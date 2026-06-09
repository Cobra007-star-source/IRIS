// =============================================================================
// slow_schema.cpp
// SlowSchema builder: index $id/$ref resources and precompile regex patterns
// =============================================================================
#include "iris/slow_schema.hpp"

#include <cctype>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#if defined(IRIS_HAVE_RE2)
    #include <re2/re2.h>
#endif

namespace iris {

// -----------------------------------------------------------------------------
// CompiledRegex: engine selected at build time
// -----------------------------------------------------------------------------
struct CompiledRegex {
#if defined(IRIS_HAVE_RE2)
    std::unique_ptr<re2::RE2> re;
    explicit CompiledRegex(std::unique_ptr<re2::RE2> r) : re(std::move(r)) {}
    [[nodiscard]] bool ok() const noexcept { return re && re->ok(); }
    [[nodiscard]] bool partial_match(std::string_view text) const noexcept {
        if (!re) return false;
        return re2::RE2::PartialMatch(re2::StringPiece(text.data(), text.size()), *re);
    }
#else
    std::regex re;
    bool ok_flag = false;
    explicit CompiledRegex(std::regex r) : re(std::move(r)), ok_flag(true) {}
    [[nodiscard]] bool ok() const noexcept { return ok_flag; }
    [[nodiscard]] bool partial_match(std::string_view text) const noexcept {
        try { return std::regex_search(text.begin(), text.end(), re); }
        catch (...) { return false; }
    }
#endif
};

void CompiledRegexDeleter::operator()(CompiledRegex* p) const noexcept {
    delete p;
}

bool slow_path_has_re2() noexcept {
#if defined(IRIS_HAVE_RE2)
    return true;
#else
    return false;
#endif
}

SlowSchema::SlowSchema()  = default;
SlowSchema::~SlowSchema() = default;
SlowSchema::SlowSchema(SlowSchema&&) noexcept            = default;
SlowSchema& SlowSchema::operator=(SlowSchema&&) noexcept = default;

namespace {

// -----------------------------------------------------------------------------
// JSON Pointer utilities
// -----------------------------------------------------------------------------
std::string unescape_ptr(std::string_view tok) {
    std::string r;
    r.reserve(tok.size());
    for (std::size_t i = 0; i < tok.size(); ++i) {
        if (tok[i] == '~' && i + 1 < tok.size()) {
            if (tok[i+1] == '0') { r.push_back('~'); ++i; continue; }
            if (tok[i+1] == '1') { r.push_back('/'); ++i; continue; }
        }
        r.push_back(tok[i]);
    }
    return r;
}

std::string escape_ptr_token(std::string_view k) {
    std::string esc;
    esc.reserve(k.size() + 4);
    for (char c : k) {
        if      (c == '~') esc += "~0";
        else if (c == '/') esc += "~1";
        else                esc.push_back(c);
    }
    return esc;
}

const JsonValue* resolve_ptr(const JsonValue& root, std::string_view ptr) {
    if (ptr.empty()) return &root;
    if (ptr.front() != '/') return nullptr;
    const JsonValue* cur = &root;
    std::size_t i = 1;
    while (i <= ptr.size()) {
        std::size_t j = ptr.find('/', i);
        if (j == std::string_view::npos) j = ptr.size();
        std::string tok = unescape_ptr(ptr.substr(i, j - i));
        if (cur->is_object()) {
            const JsonValue* next = cur->find(tok);
            if (!next) return nullptr;
            cur = next;
        } else if (cur->is_array()) {
            std::size_t idx = 0;
            for (char c : tok) {
                if (c < '0' || c > '9') return nullptr;
                idx = idx * 10 + (c - '0');
            }
            if (idx >= cur->as_array().size()) return nullptr;
            cur = &cur->as_array()[idx];
        } else {
            return nullptr;
        }
        i = j + 1;
    }
    return cur;
}

// -----------------------------------------------------------------------------
// URI utilities
// -----------------------------------------------------------------------------
// scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
bool uri_has_scheme(std::string_view r) {
    if (r.empty() || !std::isalpha(static_cast<unsigned char>(r[0]))) return false;
    for (std::size_t i = 1; i < r.size(); ++i) {
        char c = r[i];
        if (c == ':') return i > 0;
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '+' && c != '-' && c != '.') return false;
    }
    return false;
}

void split_uri(std::string_view uri, std::string& abs_part, std::string& frag) {
    auto h = uri.find('#');
    if (h == std::string_view::npos) {
        abs_part.assign(uri.data(), uri.size());
        frag.clear();
    } else {
        abs_part.assign(uri.data(), h);
        frag.assign(uri.data() + h + 1, uri.size() - h - 1);
    }
}

// RFC 3986 §5.2.4 remove_dot_segments: normalize ./ and ../
// Input is the path part (with or without leading '/'); returns normalized string.
std::string remove_dot_segments(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        // skip "../" or "./" at start of input
        if (input.compare(i, 3, "../") == 0) { i += 3; continue; }
        if (input.compare(i, 2, "./")  == 0) { i += 2; continue; }
        // "/./" → "/", "/." at end → "/"
        if (input.compare(i, 3, "/./") == 0) { i += 2; continue; }
        if (i + 2 == input.size() && input.compare(i, 2, "/.") == 0) {
            out.push_back('/'); i += 2; continue;
        }
        // "/../" → "/" (also pop last segment from out)
        if (input.compare(i, 4, "/../") == 0) {
            auto p = out.rfind('/');
            if (p != std::string::npos) out.resize(p);
            i += 3; continue;
        }
        if (i + 3 == input.size() && input.compare(i, 3, "/..") == 0) {
            auto p = out.rfind('/');
            if (p != std::string::npos) out.resize(p);
            out.push_back('/'); i += 3; continue;
        }
        // lone "." or ".."
        if ((input.size() - i == 1 && input[i] == '.') ||
            (input.size() - i == 2 && input.compare(i, 2, "..") == 0)) {
            i = input.size(); continue;
        }
        // copy one segment: from current position to next '/' (exclusive)
        std::size_t next_slash = input.find('/', i + 1);
        if (next_slash == std::string_view::npos) next_slash = input.size();
        out.append(input.data() + i, next_slash - i);
        i = next_slash;
    }
    return out;
}

// Simplified RFC 3986 reference resolution; covers all forms in the JSON Schema test suite:
//   - ref = ""           → base
//   - ref = "#frag"      → non-fragment part of base + "#frag"
//   - ref = "/abs/path"  → scheme+authority of base + ref
//   - ref = "scheme:..." → ref (absolute)
//   - ref = "rel"        → last segment of base path replaced by ref
// After resolution, run remove_dot_segments once more.
std::string uri_resolve(std::string_view base, std::string_view ref) {
    if (ref.empty()) return std::string(base);

    // ref starts with '#': keep abs part of base, replace fragment
    if (ref[0] == '#') {
        auto h = base.find('#');
        std::string out(base.substr(0, h));
        out.append(ref.data(), ref.size());
        return out;
    }

    if (uri_has_scheme(ref)) return std::string(ref);

    // parse scheme://authority + path from base
    std::string out;
    std::string_view base_nofrag = base;
    {
        auto h = base.find('#');
        if (h != std::string_view::npos) base_nofrag = base.substr(0, h);
    }

    auto scheme_end = base_nofrag.find("://");
    std::string_view scheme_authority;   // "scheme://authority"
    std::string_view base_path;          // includes leading '/'
    if (scheme_end != std::string_view::npos) {
        auto auth_start = scheme_end + 3;
        auto path_start = base_nofrag.find('/', auth_start);
        if (path_start == std::string_view::npos) {
            scheme_authority = base_nofrag;
            base_path = "/";
        } else {
            scheme_authority = base_nofrag.substr(0, path_start);
            base_path = base_nofrag.substr(path_start);
        }
    } else {
        // base has no scheme: treat as plain path
        scheme_authority = std::string_view{};
        base_path = base_nofrag;
    }

    // strip fragment; normalization applies only to path
    std::string_view ref_nofrag = ref;
    std::string_view ref_frag;
    {
        auto h = ref.find('#');
        if (h != std::string_view::npos) {
            ref_nofrag = ref.substr(0, h);
            ref_frag   = ref.substr(h);  // includes '#'
        }
    }

    std::string merged_path;
    if (!ref_nofrag.empty() && ref_nofrag[0] == '/') {
        merged_path.assign(ref_nofrag);
    } else {
        // relative path: drop last segment of base_path, then append ref_nofrag
        auto last_slash = base_path.rfind('/');
        if (last_slash == std::string_view::npos) last_slash = 0;
        merged_path.append(base_path.data(), last_slash + 1);
        merged_path.append(ref_nofrag);
    }
    std::string normalized = remove_dot_segments(merged_path);
    out.append(scheme_authority.data(), scheme_authority.size());
    out.append(normalized);
    out.append(ref_frag.data(), ref_frag.size());
    return out;
}

// URL-decode: restore "%25" / "%2F" etc. JSON Schema $ref strings may be percent-
// encoded (e.g. '%' in JSON Pointer); decode before pointer resolution.
std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c)->int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex(s[i+1]), l = hex(s[i+2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2; continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

struct CompileError {
    std::string msg;
};

// -----------------------------------------------------------------------------
// RE2 long-name Unicode class preprocessing
// JSON Schema tests use \p{Letter}; RE2 only accepts short \p{L}. Plain-text replacement.
// -----------------------------------------------------------------------------
std::string preprocess_regex(std::string_view in) {
    // (long_name, short_name)
    static const std::pair<std::string_view, std::string_view> kTable[] = {
        {"Letter", "L"},
        {"Lowercase_Letter", "Ll"},
        {"Uppercase_Letter", "Lu"},
        {"Titlecase_Letter", "Lt"},
        {"Modifier_Letter", "Lm"},
        {"Other_Letter", "Lo"},
        {"Cased_Letter", "L"},          // approximate
        {"Mark", "M"},
        {"Spacing_Mark", "Mc"},
        {"Nonspacing_Mark", "Mn"},
        {"Enclosing_Mark", "Me"},
        {"Number", "N"},
        {"Decimal_Number", "Nd"},
        {"Letter_Number", "Nl"},
        {"Other_Number", "No"},
        {"Punctuation", "P"},
        {"Connector_Punctuation", "Pc"},
        {"Dash_Punctuation", "Pd"},
        {"Open_Punctuation", "Ps"},
        {"Close_Punctuation", "Pe"},
        {"Initial_Punctuation", "Pi"},
        {"Final_Punctuation", "Pf"},
        {"Other_Punctuation", "Po"},
        {"Symbol", "S"},
        {"Math_Symbol", "Sm"},
        {"Currency_Symbol", "Sc"},
        {"Modifier_Symbol", "Sk"},
        {"Other_Symbol", "So"},
        {"Separator", "Z"},
        {"Space_Separator", "Zs"},
        {"Line_Separator", "Zl"},
        {"Paragraph_Separator", "Zp"},
        {"Control", "Cc"},
        {"Format", "Cf"},
        {"Surrogate", "Cs"},
        {"Private_Use", "Co"},
        {"Unassigned", "Cn"},
    };

    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ) {
        // detect "\p{...}" or "\P{...}"
        if (in[i] == '\\' && i + 2 < in.size() &&
            (in[i+1] == 'p' || in[i+1] == 'P') && in[i+2] == '{') {
            std::size_t end = in.find('}', i + 3);
            if (end != std::string_view::npos) {
                std::string_view name = in.substr(i + 3, end - (i + 3));
                std::string_view repl = name;
                for (auto& [lname, sname] : kTable) {
                    if (lname == name) { repl = sname; break; }
                }
                out.push_back('\\');
                out.push_back(in[i+1]);
                out.push_back('{');
                out.append(repl.data(), repl.size());
                out.push_back('}');
                i = end + 1;
                continue;
            }
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

std::unique_ptr<CompiledRegex, CompiledRegexDeleter> compile_regex(std::string_view pat) {
#if defined(IRIS_HAVE_RE2)
    std::string fixed = preprocess_regex(pat);
    re2::RE2::Options opt;
    opt.set_log_errors(false);
    opt.set_max_mem(8 << 20);   // 8 MiB DFA cache
    auto re = std::make_unique<re2::RE2>(re2::StringPiece(fixed.data(), fixed.size()), opt);
    if (!re->ok()) throw CompileError{"invalid regex: " + re->error()};
    return std::unique_ptr<CompiledRegex, CompiledRegexDeleter>(
        new CompiledRegex(std::move(re)));
#else
    try {
        std::regex tmp(std::string(pat),
                       std::regex::ECMAScript | std::regex::optimize);
        return std::unique_ptr<CompiledRegex, CompiledRegexDeleter>(
            new CompiledRegex(std::move(tmp)));
    } catch (const std::regex_error& e) {
        throw CompileError{std::string("invalid regex: ") + e.what()};
    }
#endif
}

void collect_patterns(const JsonValue& node,
                      std::unordered_map<const JsonValue*,
                          std::unique_ptr<CompiledRegex, CompiledRegexDeleter>>& cache) {
    if (node.is_object()) {
        for (auto& [k, v] : node.as_object()) {
            if (k == "pattern" && v.is_string()) {
                cache[&v] = compile_regex(v.as_string());
            } else if (k == "patternProperties" && v.is_object()) {
                for (auto& [pk, pv] : v.as_object()) {
                    cache[&pv] = compile_regex(pk);
                }
            }
            collect_patterns(v, cache);
        }
    } else if (node.is_array()) {
        for (auto& v : node.as_array()) collect_patterns(v, cache);
    }
}

// -----------------------------------------------------------------------------
// Indexing: walk tree along $id stack, register resources & node_base
// -----------------------------------------------------------------------------
void index_subschema(const JsonValue& node,
                     const std::string& base_uri,
                     const std::string& ptr_from_doc_root,
                     SlowSchema& s);

void index_object_subschemas(const JsonValue& node,
                             const std::string& base_uri,
                             const std::string& ptr_from_doc_root,
                             SlowSchema& s) {
    const auto& obj = node.as_object();

    // 1) Compute effective base URI for this node ($id may be absolute or relative).
    std::string my_base = base_uri;
    const JsonValue* id_v = node.find("$id");
    if (id_v && id_v->is_string()) {
        const std::string& id_str = id_v->as_string();
        // ignore $id with fragment (not allowed by spec)
        if (id_str.find('#') == std::string::npos) {
            my_base = uri_resolve(base_uri, id_str);
        }
    }

    // 2) Register this node in resources under several keys:
    //    (a) JSON Pointer key (base + "#" + ptr); empty ptr => "<base>#"
    //    (b) base itself (only nodes with $id) => "<my_base>"
    //    (c) anchor / dynamicAnchor → "<my_base>#<anchor>"
    //    (d) legacy same-document keys (refs table): JSON Pointer / "anchor:..."
    {
        std::string key_ptr = base_uri;
        key_ptr.push_back('#');
        key_ptr.append(ptr_from_doc_root);
        if (!s.resources.count(key_ptr)) s.resources[key_ptr] = &node;
        // legacy lookup: "<#path>" without base
        if (base_uri.empty()) {
            std::string legacy = "#";
            legacy.append(ptr_from_doc_root);
            if (!s.resources.count(legacy)) s.resources[legacy] = &node;
            if (!s.refs.count(ptr_from_doc_root)) s.refs[ptr_from_doc_root] = &node;
        }
    }
    if (id_v && id_v->is_string()) {
        // $id registers whole subschema node (base# fragment form)
        if (!s.resources.count(my_base)) s.resources[my_base] = &node;
        // also allow "<my_base>#" to match
        std::string with_hash = my_base + "#";
        if (!s.resources.count(with_hash)) s.resources[with_hash] = &node;
    }
    for (auto& [k, v] : obj) {
        if ((k == "$anchor" || k == "$dynamicAnchor") && v.is_string()) {
            const std::string& a = v.as_string();
            std::string k1 = my_base;
            k1.push_back('#');
            k1.append(a);
            if (!s.resources.count(k1)) s.resources[k1] = &node;
            // legacy compatibility
            std::string lk = "anchor:";
            lk.append(a);
            if (!s.refs.count(lk)) s.refs[lk] = &node;
        }
    }

    // 3) Record base for this subschema (used by $ref resolution)
    s.node_base[&node] = my_base;

    // 4) Recurse children. Child JSON Pointer = ptr_from_doc_root + "/" + escape(key).
    //    base follows current my_base; ptr is in-document. When this node
    //    has $id, ptr resets in new document view; base_uri changes, so both
    //    "new base + new ptr" and "old base + old ptr" locate the same node — register both.
    bool reset_ptr = (id_v && id_v->is_string());
    std::string child_doc_root_ptr;     // ptr_from_doc_root in current my_base view
    if (!reset_ptr) child_doc_root_ptr = ptr_from_doc_root;

    for (auto& [k, v] : obj) {
        std::string esc = escape_ptr_token(k);
        std::string child_ptr_old = ptr_from_doc_root;
        child_ptr_old.push_back('/');
        child_ptr_old.append(esc);

        std::string child_ptr_new = child_doc_root_ptr;
        child_ptr_new.push_back('/');
        child_ptr_new.append(esc);

        // register each child under old base (legacy "#/path" lookup)
        // only when base_uri != my_base (this node reset $id); else redundant.
        if (reset_ptr && v.is_object()) {
            // same node reachable via old base + old ptr
            std::string key_old = base_uri;
            key_old.push_back('#');
            key_old.append(child_ptr_old);
            if (!s.resources.count(key_old)) s.resources[key_old] = &v;
        }
        index_subschema(v, my_base, child_ptr_new, s);
    }
}

void index_subschema(const JsonValue& node,
                     const std::string& base_uri,
                     const std::string& ptr_from_doc_root,
                     SlowSchema& s) {
    if (node.is_object()) {
        index_object_subschemas(node, base_uri, ptr_from_doc_root, s);
    } else if (node.is_array()) {
        const auto& arr = node.as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            std::ostringstream oss;
            oss << ptr_from_doc_root << '/' << i;
            index_subschema(arr[i], base_uri, oss.str(), s);
        }
    }
}

}  // namespace

// =============================================================================
// Public API (called from slow_eval.cpp via extern)
// =============================================================================

// Resolve $ref: current_base is effective base URI at reference site ($id stack in eval).
// 1. Resolve ref to absolute URI per RFC 3986 (may still be "#fragment" form).
// 2. Look up resources by full URI first.
// 3. On miss, look up doc root by abs_part, then resolve fragment (pointer / anchor).
const JsonValue* slow_resolve_ref_uri(const SlowSchema& s,
                                      std::string_view current_base,
                                      std::string_view ref) {
    if (ref.empty()) return nullptr;

    // compute absolute URI (may include fragment)
    std::string resolved = uri_resolve(current_base, ref);

    // 1) direct hit (full URI, including #anchor / #/pointer)
    {
        // URL-decode fragment portion
        auto h = resolved.find('#');
        std::string lookup = resolved;
        if (h != std::string::npos) {
            std::string frag = url_decode(std::string_view(resolved).substr(h + 1));
            lookup.assign(resolved, 0, h + 1);
            lookup.append(frag);
        }
        auto it = s.resources.find(lookup);
        if (it != s.resources.end()) return it->second;
        if (lookup != resolved) {
            auto it2 = s.resources.find(resolved);
            if (it2 != s.resources.end()) return it2->second;
        }
    }

    // 2) split abs + frag, base-only doc lookup, fragment via JSON Pointer
    std::string abs_part, frag;
    split_uri(resolved, abs_part, frag);

    const JsonValue* doc = nullptr;
    {
        auto it = s.resources.find(abs_part);
        if (it != s.resources.end()) doc = it->second;
    }
    if (!doc && abs_part.empty()) doc = &s.root;

    if (!doc) return nullptr;
    if (frag.empty()) return doc;

    std::string decoded = url_decode(frag);
    if (!decoded.empty() && decoded[0] == '/') {
        return resolve_ptr(*doc, decoded);
    }
    // anchor form: assemble "<abs_part>#<anchor>" and look up again
    std::string anchor_key = abs_part;
    anchor_key.push_back('#');
    anchor_key.append(decoded);
    auto it = s.resources.find(anchor_key);
    if (it != s.resources.end()) return it->second;
    return nullptr;
}

// Legacy API (early slow_eval.cpp).
const JsonValue* slow_resolve_ref_impl(const SlowSchema& s, std::string_view ref) {
    return slow_resolve_ref_uri(s, s.primary_base, ref);
}

// Expose base lookup: given subschema node pointer, return effective base URI.
const std::string* slow_node_base(const SlowSchema& s, const JsonValue* node) noexcept {
    auto it = s.node_base.find(node);
    if (it == s.node_base.end()) return nullptr;
    return &it->second;
}

const CompiledRegex* slow_lookup_regex(const SlowSchema& s, const JsonValue* node) noexcept {
    auto it = s.regex_cache.find(node);
    if (it == s.regex_cache.end()) return nullptr;
    return it->second.get();
}

bool slow_regex_match(const CompiledRegex& r, std::string_view text) noexcept {
    return r.partial_match(text);
}

bool slow_regex_validate(std::string_view pattern) noexcept {
    try {
#if defined(IRIS_HAVE_RE2)
        std::string fixed = preprocess_regex(pattern);
        re2::RE2 re(re2::StringPiece(fixed.data(), fixed.size()), re2::RE2::Quiet);
        return re.ok();
#else
        std::regex re(std::string(pattern), std::regex::ECMAScript);
        (void)re;
        return true;
#endif
    } catch (...) { return false; }
}

bool slow_regex_match_inline(std::string_view pattern, std::string_view text) noexcept {
    try {
#if defined(IRIS_HAVE_RE2)
        std::string fixed = preprocess_regex(pattern);
        re2::RE2 re(re2::StringPiece(fixed.data(), fixed.size()), re2::RE2::Quiet);
        if (!re.ok()) return false;
        return re2::RE2::PartialMatch(re2::StringPiece(text.data(), text.size()), re);
#else
        std::regex re(std::string(pattern), std::regex::ECMAScript);
        return std::regex_search(text.begin(), text.end(), re);
#endif
    } catch (...) { return false; }
}

// =============================================================================
// Public API: compile + remote document injection
// =============================================================================
// Pre-register official JSON Schema metaschema URIs (and vocabulary sub-schemas) so
// $ref to them always pass. Metaschema validates schemas; for instance validation,
// referencing metaschema is equivalent to a `true` schema.
// Single static sentinel node; all metaschema URIs map to it.
const JsonValue& metaschema_sentinel() {
    static JsonValue v = JsonValue::make_bool(true);
    return v;
}

void preregister_metaschemas(SlowSchema& s) {
    static const char* kUris[] = {
        // 2020-12
        "https://json-schema.org/draft/2020-12/schema",
        "https://json-schema.org/draft/2020-12/meta/core",
        "https://json-schema.org/draft/2020-12/meta/applicator",
        "https://json-schema.org/draft/2020-12/meta/validation",
        "https://json-schema.org/draft/2020-12/meta/meta-data",
        "https://json-schema.org/draft/2020-12/meta/format-annotation",
        "https://json-schema.org/draft/2020-12/meta/format-assertion",
        "https://json-schema.org/draft/2020-12/meta/content",
        "https://json-schema.org/draft/2020-12/meta/unevaluated",
        // 2019-09 (for safety)
        "https://json-schema.org/draft/2019-09/schema",
        "https://json-schema.org/draft/2019-09/meta/core",
        "https://json-schema.org/draft/2019-09/meta/applicator",
        "https://json-schema.org/draft/2019-09/meta/validation",
        "https://json-schema.org/draft/2019-09/meta/meta-data",
        "https://json-schema.org/draft/2019-09/meta/format",
        "https://json-schema.org/draft/2019-09/meta/content",
        // older drafts
        "http://json-schema.org/draft-07/schema#",
        "http://json-schema.org/draft-07/schema",
        "http://json-schema.org/draft-06/schema#",
        "http://json-schema.org/draft-06/schema",
        "http://json-schema.org/draft-04/schema#",
    };
    for (const char* u : kUris) {
        if (!s.resources.count(u)) s.resources[u] = &metaschema_sentinel();
    }
}

SlowSchemaBuildResult compile_slow_schema(std::string_view schema_json) {
    SlowSchemaBuildResult out;
    auto parsed = parse_json(schema_json);
    if (!parsed.ok) {
        out.diagnostic = "schema parse error: " + parsed.diagnostic;
        return out;
    }
    auto schema = std::make_unique<SlowSchema>();
    schema->root = std::move(parsed.value);

    // Primary document base: root.$id if present and has no fragment
    if (schema->root.is_object()) {
        if (const JsonValue* id_v = schema->root.find("$id")) {
            if (id_v->is_string() && id_v->as_string().find('#') == std::string::npos) {
                schema->primary_base = id_v->as_string();
            }
        }
    }

    index_subschema(schema->root, schema->primary_base, std::string{}, *schema);
    preregister_metaschemas(*schema);

    try {
        collect_patterns(schema->root, schema->regex_cache);
    } catch (const CompileError& e) {
        out.diagnostic = e.msg;
        return out;
    }

    out.schema = std::move(schema);
    out.ok     = true;
    return out;
}

bool slow_schema_add_remote(SlowSchema& s, std::string uri, std::string_view json) noexcept {
    if (uri.empty()) return false;
    auto parsed = parse_json(json);
    if (!parsed.ok) return false;

    auto root_holder = std::make_unique<JsonValue>(std::move(parsed.value));
    JsonValue* root_ptr = root_holder.get();

    // Remote doc $id may differ from caller uri; spec prefers $id; for safety
    // we use caller-supplied uri as base (stable: registered that way externally).
    std::string base = std::move(uri);
    if (root_ptr->is_object()) {
        if (const JsonValue* id_v = root_ptr->find("$id")) {
            if (id_v->is_string() &&
                id_v->as_string().find('#') == std::string::npos) {
                // note: caller uri remains primary key; $id uri also registered as alias
                if (!s.resources.count(id_v->as_string()))
                    s.resources[id_v->as_string()] = root_ptr;
            }
        }
    }

    // register root itself
    s.resources[base] = root_ptr;
    s.resources[base + "#"] = root_ptr;

    index_subschema(*root_ptr, base, std::string{}, s);

    try {
        collect_patterns(*root_ptr, s.regex_cache);
    } catch (...) {
        // invalid regex in remote doc: ignore; main flow continues
    }

    s.remote_roots.push_back(std::move(root_holder));
    return true;
}

}  // namespace iris
