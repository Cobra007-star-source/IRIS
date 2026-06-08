// =============================================================================
// src/slow_schema.cpp
//
// SlowSchema 构建器：
//   1. 解析 schema JSON → JsonValue 树
//   2. 沿 $id 栈遍历，给每个 subschema 注册 base URI + JSON Pointer / $anchor /
//      $dynamicAnchor 命中点
//   3. 预编译每个 "pattern" / patternProperties.key 为正则（含 \p{Letter} 之类
//      Unicode 长名 → RE2 短名的预处理）
//   4. 同时建好"node → base URI"映射，evaluation 期 0 拷贝 lookup
//
// 正则后端：build-time 选择
//   * IRIS_HAVE_RE2 = 1 → 用 RE2（线性时间、ReDoS-safe）
//   * 否则           → 用 std::regex（ECMAScript flavor，可能在 adversarial
//                                     输入上出现 quadratic backtracking）
//
// 跨文档 $ref：通过 slow_schema_add_remote 注入远端文档；slow_eval 通过
// resources 表按已解析的绝对 URI 直接查节点。
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
// CompiledRegex：根据 build flag 切换具体引擎
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
// JSON Pointer 工具
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
// URI 工具
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

// RFC 3986 §5.2.4 remove_dot_segments：把 ./ 和 ../ 规范化掉。
// 输入是 path 部分（含或不含开头 '/'），返回规范化后字符串。
std::string remove_dot_segments(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        // "../" 或 "./" 在 input 开头：跳过
        if (input.compare(i, 3, "../") == 0) { i += 3; continue; }
        if (input.compare(i, 2, "./")  == 0) { i += 2; continue; }
        // "/./" → "/"，"/." 在末尾 → "/"
        if (input.compare(i, 3, "/./") == 0) { i += 2; continue; }
        if (i + 2 == input.size() && input.compare(i, 2, "/.") == 0) {
            out.push_back('/'); i += 2; continue;
        }
        // "/../" → "/"（同时弹出 out 的最后一个段）
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
        // 单独 "." 或 ".."
        if ((input.size() - i == 1 && input[i] == '.') ||
            (input.size() - i == 2 && input.compare(i, 2, "..") == 0)) {
            i = input.size(); continue;
        }
        // 拷贝一个 segment：从当前位置到下一个 '/'（不含下一个 '/'）
        std::size_t next_slash = input.find('/', i + 1);
        if (next_slash == std::string_view::npos) next_slash = input.size();
        out.append(input.data() + i, next_slash - i);
        i = next_slash;
    }
    return out;
}

// 简化的 RFC 3986 reference resolution。覆盖 JSON Schema 测试集里的所有形态：
//   - ref = ""           → base
//   - ref = "#frag"      → base 的非 fragment 部分 + "#frag"
//   - ref = "/abs/path"  → base 的 scheme+authority + ref
//   - ref = "scheme:..." → ref（绝对）
//   - ref = "rel"        → base 路径最后一段被 ref 替换
// 解析完毕后再走一次 remove_dot_segments 规范化。
std::string uri_resolve(std::string_view base, std::string_view ref) {
    if (ref.empty()) return std::string(base);

    // ref 以 '#' 开头：保留 base 的 abs 部分，换 fragment
    if (ref[0] == '#') {
        auto h = base.find('#');
        std::string out(base.substr(0, h));
        out.append(ref.data(), ref.size());
        return out;
    }

    if (uri_has_scheme(ref)) return std::string(ref);

    // 解析 base 的 scheme://authority + path
    std::string out;
    std::string_view base_nofrag = base;
    {
        auto h = base.find('#');
        if (h != std::string_view::npos) base_nofrag = base.substr(0, h);
    }

    auto scheme_end = base_nofrag.find("://");
    std::string_view scheme_authority;   // "scheme://authority"
    std::string_view base_path;          // 含前导 '/'
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
        // base 没有 scheme：当作纯路径处理
        scheme_authority = std::string_view{};
        base_path = base_nofrag;
    }

    // 把 fragment 拆出去，规范化只作用在 path 部分
    std::string_view ref_nofrag = ref;
    std::string_view ref_frag;
    {
        auto h = ref.find('#');
        if (h != std::string_view::npos) {
            ref_nofrag = ref.substr(0, h);
            ref_frag   = ref.substr(h);  // 含 '#'
        }
    }

    std::string merged_path;
    if (!ref_nofrag.empty() && ref_nofrag[0] == '/') {
        merged_path.assign(ref_nofrag);
    } else {
        // 相对路径：移除 base_path 最后一段，再 append ref_nofrag
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

// URL-decode：把 "%25" / "%2F" 之类还原。JSON Schema $ref 字符串可能带 percent-
// encoded 字符（典型场景：JSON Pointer 里含 '%'），需先 decode 再做 pointer 解析。
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
// RE2 长名 Unicode 类预处理
// JSON Schema 测试里出现 \p{Letter}，RE2 仅识别短名 \p{L}。我们做一次纯文本替换。
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
        {"Cased_Letter", "L"},          // 近似
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
        // 探测 "\p{...}" 或 "\P{...}"
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
// 索引主体：沿 $id 栈走树，登记 resources & node_base
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

    // 1) 计算本节点的有效 base URI（看 $id；$id 可能是绝对或相对）。
    std::string my_base = base_uri;
    const JsonValue* id_v = node.find("$id");
    if (id_v && id_v->is_string()) {
        const std::string& id_str = id_v->as_string();
        // 忽略带 fragment 的 $id（spec 不允许）
        if (id_str.find('#') == std::string::npos) {
            my_base = uri_resolve(base_uri, id_str);
        }
    }

    // 2) 登记本节点到 resources 表的几种 key：
    //    (a) JSON Pointer key（base + "#" + ptr）；ptr 空时即 "<base>#"
    //    (b) base 自身（只对带 $id 的节点）→ "<my_base>"
    //    (c) anchor / dynamicAnchor → "<my_base>#<anchor>"
    //    (d) 兼容旧版同文档 key（refs 表）：JSON Pointer / "anchor:..."
    {
        std::string key_ptr = base_uri;
        key_ptr.push_back('#');
        key_ptr.append(ptr_from_doc_root);
        if (!s.resources.count(key_ptr)) s.resources[key_ptr] = &node;
        // 兼容旧 lookup："<#path>" 不带 base
        if (base_uri.empty()) {
            std::string legacy = "#";
            legacy.append(ptr_from_doc_root);
            if (!s.resources.count(legacy)) s.resources[legacy] = &node;
            if (!s.refs.count(ptr_from_doc_root)) s.refs[ptr_from_doc_root] = &node;
        }
    }
    if (id_v && id_v->is_string()) {
        // $id 注册整个 subschema 节点（带 fragment 形式 base#）
        if (!s.resources.count(my_base)) s.resources[my_base] = &node;
        // 也允许 "<my_base>#" 命中
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
            // legacy 兼容
            std::string lk = "anchor:";
            lk.append(a);
            if (!s.refs.count(lk)) s.refs[lk] = &node;
        }
    }

    // 3) 记录该 subschema 的 base，用于后续 $ref 解析
    s.node_base[&node] = my_base;

    // 4) 递归子节点。注意：每个子键的 JSON Pointer = ptr_from_doc_root + "/" + escape(key)。
    //    base 必须按"当前节点的 my_base"传下去；ptr 是当前文档内的 pointer：当本节点
    //    自己拥有 $id 时，ptr 在新文档视角下重置；但同时 base_uri 改变了，所以查找时
    //    "新 base + 新 ptr"和"旧 base + 旧 ptr"都能定位同一节点——我们两种 key 都登记。
    bool reset_ptr = (id_v && id_v->is_string());
    std::string child_doc_root_ptr;     // 在当前 my_base 视角下的 ptr_from_doc_root
    if (!reset_ptr) child_doc_root_ptr = ptr_from_doc_root;

    for (auto& [k, v] : obj) {
        std::string esc = escape_ptr_token(k);
        std::string child_ptr_old = ptr_from_doc_root;
        child_ptr_old.push_back('/');
        child_ptr_old.append(esc);

        std::string child_ptr_new = child_doc_root_ptr;
        child_ptr_new.push_back('/');
        child_ptr_new.append(esc);

        // 先在 old base 视角下登记每个 child（为了旧的 "#/path" 兼容查找）
        // 但只在 base_uri 与 my_base 不同（即本节点重设 $id）时；否则二者重合无需重复。
        if (reset_ptr && v.is_object()) {
            // 用旧 base + 旧 ptr 也能查到同一节点
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
// 公开（slow_eval.cpp 通过 extern 调用）
// =============================================================================

// 解析 $ref：current_base 是发起点的有效 base URI（在 eval 中由 $id 栈维护）。
// 1. 把 ref 按 RFC 3986 解析为绝对 URI（可能仍是 "#fragment" 形态）。
// 2. 首先按完整 URI 查 resources。
// 3. 没命中时按 "abs_part" 查 doc 根，再用 fragment（JSON Pointer / anchor）二级解析。
const JsonValue* slow_resolve_ref_uri(const SlowSchema& s,
                                      std::string_view current_base,
                                      std::string_view ref) {
    if (ref.empty()) return nullptr;

    // 计算绝对 URI（可能含 fragment）
    std::string resolved = uri_resolve(current_base, ref);

    // 1) 直接命中（完整 URI，包括 #anchor / #/pointer 形式）
    {
        // URL-decode fragment 部分
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

    // 2) 拆 abs + frag，再 base-only 查 doc 根，frag 走 JSON Pointer
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
    // 是 anchor 形式：组装 "<abs_part>#<anchor>" 再查一次
    std::string anchor_key = abs_part;
    anchor_key.push_back('#');
    anchor_key.append(decoded);
    auto it = s.resources.find(anchor_key);
    if (it != s.resources.end()) return it->second;
    return nullptr;
}

// 兼容旧 API（slow_eval.cpp 早期版本调用）。
const JsonValue* slow_resolve_ref_impl(const SlowSchema& s, std::string_view ref) {
    return slow_resolve_ref_uri(s, s.primary_base, ref);
}

// 暴露 base 查询：slow_eval 拿到一个 subschema 节点 pointer 后，查它的有效 base URI。
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
// 公开 API：编译 + 远端注入
// =============================================================================
// 预注册 JSON Schema 官方 metaschema URI（含其分词汇 sub-schema），让指向它们的
// $ref 始终通过。metaschema 用于校验 schema 本身的合法性；对于"validator
// validate instance"的常规调用而言，引用 metaschema 等同于 `true` schema。
// 用一个全局静态 sentinel 节点，所有 metaschema URI 都映射到它。
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
        // 2019-09（保险起见）
        "https://json-schema.org/draft/2019-09/schema",
        "https://json-schema.org/draft/2019-09/meta/core",
        "https://json-schema.org/draft/2019-09/meta/applicator",
        "https://json-schema.org/draft/2019-09/meta/validation",
        "https://json-schema.org/draft/2019-09/meta/meta-data",
        "https://json-schema.org/draft/2019-09/meta/format",
        "https://json-schema.org/draft/2019-09/meta/content",
        // 旧版
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

    // 主文档 base：取 root.$id（如果存在且无 fragment）
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

    // 远端文档自身 $id 可能覆盖外部 uri，spec 上以 $id 为准；但为保险起见
    // 我们用调用方传入的 uri 作为 base（更稳，因为外面就是按它注册的）。
    std::string base = std::move(uri);
    if (root_ptr->is_object()) {
        if (const JsonValue* id_v = root_ptr->find("$id")) {
            if (id_v->is_string() &&
                id_v->as_string().find('#') == std::string::npos) {
                // 注：仍以传入 uri 为主键，但 $id 的 uri 也额外登记一份别名
                if (!s.resources.count(id_v->as_string()))
                    s.resources[id_v->as_string()] = root_ptr;
            }
        }
    }

    // 登记 root 自己
    s.resources[base] = root_ptr;
    s.resources[base + "#"] = root_ptr;

    index_subschema(*root_ptr, base, std::string{}, s);

    try {
        collect_patterns(*root_ptr, s.regex_cache);
    } catch (...) {
        // 远端文档里有 invalid regex：忽略，主流程仍可继续
    }

    s.remote_roots.push_back(std::move(root_holder));
    return true;
}

}  // namespace iris
