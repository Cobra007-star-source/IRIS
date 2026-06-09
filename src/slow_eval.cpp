// =============================================================================
// slow_eval.cpp
// Slow path: recursive JSON Schema 2020-12 keyword interpreter
// =============================================================================
#include "iris/slow_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace iris {

extern const JsonValue* slow_resolve_ref_impl(const SlowSchema&, std::string_view);
extern const JsonValue* slow_resolve_ref_uri(const SlowSchema&, std::string_view current_base,
                                              std::string_view ref);
extern const std::string* slow_node_base(const SlowSchema&, const JsonValue*) noexcept;
extern const JsonValue& metaschema_sentinel();

namespace {

// Parse $schema → remote metaschema → check $vocabulary for validation
// vocab. If absent/false, return true: skip all validation keywords (spec §8.1).
// (see above)
bool check_validation_vocab_disabled(const SlowSchema& s) {
    if (!s.root.is_object()) return false;
    const JsonValue* meta_url = s.root.find("$schema");
    if (!meta_url || !meta_url->is_string()) return false;
    auto it = s.resources.find(meta_url->as_string());
    if (it == s.resources.end()) return false;
    const JsonValue* meta = it->second;
    if (!meta || !meta->is_object()) return false;
    const JsonValue* voc = meta->find("$vocabulary");
    if (!voc || !voc->is_object()) return false;
    // validation vocab URIs（draft 2020-12 / 2019-09）
    static const char* kValidationUris[] = {
        "https://json-schema.org/draft/2020-12/vocab/validation",
        "https://json-schema.org/draft/2019-09/vocab/validation",
    };
    for (const char* u : kValidationUris) {
        const JsonValue* v = voc->find(u);
        if (v && v->is_bool() && v->as_bool()) return false;  // explicit true → enabled
    }
    return true;  // not declared or false → disabled
}

}  // namespace
extern const CompiledRegex* slow_lookup_regex(const SlowSchema&, const JsonValue*) noexcept;
extern bool slow_regex_match(const CompiledRegex&, std::string_view) noexcept;
extern bool slow_regex_match_inline(std::string_view, std::string_view) noexcept;
extern bool slow_regex_validate(std::string_view) noexcept;

namespace {

// -----------------------------------------------------------------------------
// Utilities: ValidationReport construction
// -----------------------------------------------------------------------------
inline ValidationReport ok_report() noexcept { return {}; }
inline ValidationReport err_report(ValidationError c) noexcept {
    ValidationReport r; r.code = c; return r;
}

// -----------------------------------------------------------------------------
// JsonValue deep equality (const / enum / uniqueItems)
// -----------------------------------------------------------------------------
bool json_eq(const JsonValue& a, const JsonValue& b) {
    using T = JsonValue::Type;
    if (a.is_number() && b.is_number()) {
        // JSON Schema: numbers equal iff mathematically equal
        double da = a.as_double(), db = b.as_double();
        return da == db;
    }
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case T::kNull:   return true;
        case T::kBool:   return a.as_bool() == b.as_bool();
        case T::kInt:    return a.as_int() == b.as_int();
        case T::kDouble: return a.as_double() == b.as_double();
        case T::kString: return a.as_string() == b.as_string();
        case T::kArray: {
            const auto& aa = a.as_array(); const auto& bb = b.as_array();
            if (aa.size() != bb.size()) return false;
            for (std::size_t i = 0; i < aa.size(); ++i)
                if (!json_eq(aa[i], bb[i])) return false;
            return true;
        }
        case T::kObject: {
            const auto& ao = a.as_object(); const auto& bo = b.as_object();
            if (ao.size() != bo.size()) return false;
            // key order not required
            for (auto& [k, v] : ao) {
                const JsonValue* m = b.find(k);
                if (!m || !json_eq(v, *m)) return false;
            }
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Unicode codepoint count
// -----------------------------------------------------------------------------
std::size_t utf8_codepoints(std::string_view s) noexcept {
    std::size_t cnt = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if      (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;  // malformed → count as 1 byte
        ++cnt;
    }
    return cnt;
}

// -----------------------------------------------------------------------------
// type keyword check
// -----------------------------------------------------------------------------
bool type_matches_one(std::string_view tn, const JsonValue& v) noexcept {
    using T = JsonValue::Type;
    if (tn == "null")    return v.type() == T::kNull;
    if (tn == "boolean") return v.type() == T::kBool;
    if (tn == "string")  return v.type() == T::kString;
    if (tn == "number")  return v.is_number();
    if (tn == "integer") {
        if (v.type() == T::kInt) return true;
        if (v.type() == T::kDouble) {
            double d = v.as_double();
            return std::isfinite(d) && std::floor(d) == d;
        }
        return false;
    }
    if (tn == "array")   return v.type() == T::kArray;
    if (tn == "object")  return v.type() == T::kObject;
    return false;
}

bool type_matches(const JsonValue& type_node, const JsonValue& instance) noexcept {
    if (type_node.is_string()) return type_matches_one(type_node.as_string(), instance);
    if (type_node.is_array()) {
        for (auto& t : type_node.as_array())
            if (t.is_string() && type_matches_one(t.as_string(), instance)) return true;
        return false;
    }
    return false;
}

// -----------------------------------------------------------------------------
// numeric keywords
// -----------------------------------------------------------------------------
bool check_multiple_of(double instance, double divisor) noexcept {
    if (divisor == 0.0) return false;
    double q = instance / divisor;
    if (!std::isfinite(q)) return false;   // overflow → never divisible
    // Allow ulp tolerance. Test suite: 0.0075/0.0001=75 but IEEE-754
    // may yield 74.999999...; use 1e-9 tolerance here.
    double r = std::round(q);
    return std::fabs(q - r) < 1e-9;
}

// -----------------------------------------------------------------------------
// Regex via opaque CompiledRegex from slow_schema.cpp
// -----------------------------------------------------------------------------
bool regex_search_partial(const SlowSchema& schema, const JsonValue* pat_node,
                          std::string_view text) noexcept {
    if (const CompiledRegex* r = slow_lookup_regex(schema, pat_node)) {
        return slow_regex_match(*r, text);
    }
    // cache miss → compile inline
    if (!pat_node->is_string()) return false;
    return slow_regex_match_inline(pat_node->as_string(), text);
}

bool regex_search_inline(std::string_view pattern, std::string_view text) noexcept {
    return slow_regex_match_inline(pattern, text);
}

// -----------------------------------------------------------------------------
// format checks (subset; draft 2020-12 default annotation; implemented as assertion)
// -----------------------------------------------------------------------------
bool is_digits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

bool format_date(std::string_view s) noexcept {
    // YYYY-MM-DD
    if (s.size() != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    if (!is_digits(s.substr(0,4)) || !is_digits(s.substr(5,2)) || !is_digits(s.substr(8,2)))
        return false;
    int m = (s[5]-'0')*10 + (s[6]-'0');
    int d = (s[8]-'0')*10 + (s[9]-'0');
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    if (m < 1 || m > 12) return false;
    if (d < 1) return false;
    int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    if (m == 2 && leap) { if (d > 29) return false; }
    else if (d > dim[m-1]) return false;
    return true;
}

bool format_time(std::string_view s) noexcept {
    // HH:MM:SS[.frac][Z|+HH:MM|-HH:MM]
    if (s.size() < 9) return false;
    if (!is_digits(s.substr(0,2)) || s[2]!=':' ||
        !is_digits(s.substr(3,2)) || s[5]!=':' ||
        !is_digits(s.substr(6,2))) return false;
    int hh = (s[0]-'0')*10 + (s[1]-'0');
    int mm = (s[3]-'0')*10 + (s[4]-'0');
    int ss = (s[6]-'0')*10 + (s[7]-'0');
    if (hh > 23 || mm > 59 || ss > 60) return false;  // 60 allows leap second
    std::size_t i = 8;
    if (i < s.size() && s[i] == '.') {
        ++i;
        std::size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start) return false;
    }
    if (i == s.size()) return false;  // timezone required
    if (s[i] == 'Z' || s[i] == 'z') { ++i; return i == s.size(); }
    if (s[i] != '+' && s[i] != '-') return false;
    if (i + 6 != s.size()) return false;
    return is_digits(s.substr(i+1,2)) && s[i+3]==':' && is_digits(s.substr(i+4,2));
}

bool format_date_time(std::string_view s) noexcept {
    auto t = s.find_first_of("Tt");
    if (t == std::string_view::npos) return false;
    return format_date(s.substr(0, t)) && format_time(s.substr(t + 1));
}

bool format_email(std::string_view s) noexcept {
    // simplified RFC 5322
    auto at = s.find('@');
    if (at == std::string_view::npos || at == 0 || at == s.size() - 1) return false;
    auto local = s.substr(0, at);
    auto host  = s.substr(at + 1);
    if (local.size() > 64) return false;
    for (char c : local) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            std::strchr(".!#$%&'*+/=?^_`{|}~-", c) == nullptr) return false;
    }
    if (host.empty() || host.front() == '.' || host.back() == '.') return false;
    if (host.find("..") != std::string_view::npos) return false;
    return true;
}

bool format_ipv4(std::string_view s) noexcept {
    int dots = 0, cur = -1;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') {
            if (cur < 0 || cur > 255) return false;
            ++dots; cur = -1;
        } else if (c >= '0' && c <= '9') {
            // no leading zeros except 0 itself
            if (cur == 0) return false;
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 255) return false;
        } else return false;
    }
    return dots == 3 && cur >= 0 && cur <= 255;
}

bool format_ipv6(std::string_view s) noexcept {
    // simplified: 8 groups of 1-4 hex, ":", one "::" compression allowed
    if (s.empty()) return false;
    std::vector<std::string_view> groups;
    bool has_dbl = false;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ':') {
            if (i + 1 < s.size() && s[i+1] == ':') {
                if (has_dbl) return false;
                has_dbl = true; groups.emplace_back(); i += 2;
            } else { groups.emplace_back(); ++i; }
        } else {
            std::size_t j = i;
            while (j < s.size() && s[j] != ':') ++j;
            if (j - i == 0 || j - i > 4) return false;
            for (std::size_t k = i; k < j; ++k) {
                if (!std::isxdigit(static_cast<unsigned char>(s[k]))) return false;
            }
            groups.emplace_back(s.substr(i, j - i));
            i = j;
        }
    }
    if (has_dbl) return groups.size() <= 8;
    return groups.size() == 8;
}

bool format_uri(std::string_view s) noexcept {
    auto colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
    if (!std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    for (std::size_t i = 1; i < colon; ++i) {
        char c = s[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '+' && c != '-' && c != '.') return false;
    }
    return true;
}

bool format_uuid(std::string_view s) noexcept {
    if (s.size() != 36) return false;
    static const int dash[] = {8, 13, 18, 23};
    int di = 0;
    for (std::size_t i = 0; i < 36; ++i) {
        if (di < 4 && i == static_cast<std::size_t>(dash[di])) {
            if (s[i] != '-') return false;
            ++di;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool format_hostname(std::string_view s) noexcept {
    if (s.empty() || s.size() > 253) return false;
    std::size_t label_start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            std::size_t len = i - label_start;
            if (len == 0 || len > 63) return false;
            char first = s[label_start];
            char last  = s[i - 1];
            if (first == '-' || last == '-') return false;
            for (std::size_t k = label_start; k < i; ++k) {
                char c = s[k];
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
            }
            label_start = i + 1;
        }
    }
    return true;
}

bool format_regex(std::string_view s) noexcept {
    return slow_regex_validate(s);
}

bool format_json_pointer(std::string_view s) noexcept {
    if (s.empty()) return true;  // "" is valid root pointer
    if (s.front() != '/') return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (s[i] == '~') {
            if (i + 1 >= s.size() || (s[i+1] != '0' && s[i+1] != '1')) return false;
            ++i;
        }
    }
    return true;
}

bool check_format(std::string_view fmt, std::string_view text) noexcept {
    if (fmt == "date")          return format_date(text);
    if (fmt == "time")          return format_time(text);
    if (fmt == "date-time")     return format_date_time(text);
    if (fmt == "email")         return format_email(text);
    if (fmt == "idn-email")     return format_email(text);
    if (fmt == "ipv4")          return format_ipv4(text);
    if (fmt == "ipv6")          return format_ipv6(text);
    if (fmt == "uri")           return format_uri(text);
    if (fmt == "uri-reference") return true;  // any string is valid uri-reference
    if (fmt == "iri")           return format_uri(text);
    if (fmt == "iri-reference") return true;
    if (fmt == "uuid")          return format_uuid(text);
    if (fmt == "hostname")      return format_hostname(text);
    if (fmt == "idn-hostname")  return format_hostname(text);
    if (fmt == "regex")         return format_regex(text);
    if (fmt == "json-pointer")  return format_json_pointer(text);
    if (fmt == "relative-json-pointer") return true;  // simplified
    if (fmt == "duration")      return !text.empty() && text.front() == 'P';
    // unknown format: spec default annotation-only → pass
    return true;
}

// -----------------------------------------------------------------------------
// Recursive evaluation
// -----------------------------------------------------------------------------
// Dynamic scope frames: chain of schema resources entered by evaluator.
// JSON Schema 2020-12 §8.2.3.2: dynamic scope is schema resources,
// each bounded by $id. $dynamicRef "#name" resolution:
//   1. static resolution to target T;
//   2. if T has $dynamicAnchor=name, dynamic mode:
//      scan dynamic_scope outer→inner for first $dynamicAnchor=name;
//      $defs entries count if inside resource boundary.
//      (continued)
// Each frame stores base URI; lookup via resources
// `<frame.base>#<anchor>`.
struct DynamicFrame {
    std::string base_uri;
};

struct EvalCtx {
    const SlowSchema& schema;
    int depth = 0;
    // current effective base URI along $id path (push/pop).
    // for $ref / $dynamicRef relative resolution per RFC 3986.
    std::string current_base;
    // dynamic scope stack (spec 8.2.3.2); pushed at eval_object_keywords entry.
    std::vector<DynamicFrame> dynamic_scope;
    static constexpr int kMaxDepth = 256;
};

// On subschema entry switch current_base; prefer indexed node_base
// (base after $id); avoids double resolve on $ref jump stacking bases.
// Returns saved base for restore on exit.
inline std::string push_id_scope_for_node(const JsonValue& node, EvalCtx& ctx) {
    std::string saved = ctx.current_base;
    if (const std::string* nb = slow_node_base(ctx.schema, &node)) {
        ctx.current_base = *nb;
    }
    return saved;
}

ValidationReport eval(const JsonValue& schema_node, const JsonValue& instance, EvalCtx& ctx);

// Properties consumed by properties/patternProperties/additionalProperties,
// for unevaluatedProperties. Simplified: only properties + patternProperties
// marked; additionalProperties vs unevaluatedProperties independent.
//
// Full impl needs annotation collection: merge evaluated sets from $ref/allOf
// subschemas using properties. Main source of unevaluatedProperties test gaps;
// v1 accepts this approximation.

ValidationReport eval_object_keywords(const JsonObject& schema_obj,
                                      const JsonValue& instance,
                                      EvalCtx& ctx);

// -----------------------------------------------------------------------------
// keyword handlers: ok means passed (including vacuous)
// -----------------------------------------------------------------------------

ValidationReport handle_const(const JsonValue& v, const JsonValue& inst, const SlowSchema& s) {
    if (s.disable_validation_vocab == 1) return ok_report();
    return json_eq(v, inst) ? ok_report() : err_report(ValidationError::kConstMismatch);
}

ValidationReport handle_enum(const JsonValue& v, const JsonValue& inst, const SlowSchema& s) {
    if (s.disable_validation_vocab == 1) return ok_report();
    if (!v.is_array()) return err_report(ValidationError::kSlowSchemaInvalid);
    for (auto& opt : v.as_array())
        if (json_eq(opt, inst)) return ok_report();
    return err_report(ValidationError::kEnumMismatch);
}

ValidationReport handle_number_kw(std::string_view k, const JsonValue& v,
                                  const JsonValue& inst, const SlowSchema& s) {
    if (s.disable_validation_vocab == 1) return ok_report();
    if (!inst.is_number()) return ok_report();  // vacuous on non-number
    if (!v.is_number())    return err_report(ValidationError::kSlowSchemaInvalid);
    double iv = inst.as_double();
    double cv = v.as_double();
    if (k == "minimum")          { if (iv < cv)  return err_report(ValidationError::kIntOutOfRange); }
    else if (k == "maximum")     { if (iv > cv)  return err_report(ValidationError::kIntOutOfRange); }
    else if (k == "exclusiveMinimum") { if (iv <= cv) return err_report(ValidationError::kIntOutOfRange); }
    else if (k == "exclusiveMaximum") { if (iv >= cv) return err_report(ValidationError::kIntOutOfRange); }
    else if (k == "multipleOf") {
        if (!check_multiple_of(iv, cv)) return err_report(ValidationError::kMultipleOf);
    }
    return ok_report();
}

ValidationReport handle_string_kw(std::string_view k, const JsonValue& v,
                                  const JsonValue& inst, const SlowSchema& schema) {
    if (schema.disable_validation_vocab == 1) return ok_report();
    if (!inst.is_string()) return ok_report();
    const auto& s = inst.as_string();
    if (k == "minLength") {
        std::size_t cp = utf8_codepoints(s);
        if (v.is_int() && cp < static_cast<std::size_t>(v.as_int()))
            return err_report(ValidationError::kStringTooShort);
    } else if (k == "maxLength") {
        std::size_t cp = utf8_codepoints(s);
        if (v.is_int() && cp > static_cast<std::size_t>(v.as_int()))
            return err_report(ValidationError::kStringTooLong);
    } else if (k == "pattern") {
        if (!v.is_string()) return err_report(ValidationError::kSlowSchemaInvalid);
        if (!regex_search_partial(schema, &v, s))
            return err_report(ValidationError::kPatternMismatch);
    } else if (k == "format") {
        // draft 2020-12: format default annotation-only.
        // format-assertion vocabulary treats as assertion; IRIS does not enable it;
        // matches reference impl — any format passes.
        // check_format() kept for one-line enable when format-assertion is on.
        (void)v; (void)s; (void)schema;
    }
    return ok_report();
}

// Annotations for unevaluatedProperties / unevaluatedItems / minContains
// and sibling-communicating keywords. Each eval(...) receives Annotations;
// handlers append consumed props/items on success.
//
// Only successful subschemas contribute annotations (spec rule);
// critical for unevaluatedItems with not cases.
struct Annotations {
    std::vector<std::string> evaluated_props;     // field names consumed by properties/patternProperties/additionalProperties
    bool                     all_items_seen = false;  // items (single-schema form)
    std::size_t              prefix_items_seen = 0;   // indices consumed by prefixItems / items-as-array
    std::vector<std::size_t> contains_indices;     // indices matched by contains
};

inline void merge_ann(Annotations& dst, const Annotations& src) {
    for (auto& p : src.evaluated_props) dst.evaluated_props.push_back(p);
    dst.all_items_seen = dst.all_items_seen || src.all_items_seen;
    dst.prefix_items_seen = std::max(dst.prefix_items_seen, src.prefix_items_seen);
    for (auto i : src.contains_indices) dst.contains_indices.push_back(i);
}

ValidationReport eval_with_ann(const JsonValue& schema_node, const JsonValue& instance,
                               EvalCtx& ctx, Annotations& ann);

// Array cluster: items / prefixItems / contains / additionalItems / minContains /
// maxContains / unevaluatedItems — joint eval for annotation consistency.
ValidationReport process_array_cluster(const JsonObject& schema_obj,
                                       const JsonValue& inst,
                                       EvalCtx& ctx, Annotations& ann);

// Object cluster: properties / patternProperties / additionalProperties /
// unevaluatedProperties / required / dependentRequired / dependentSchemas /
// propertyNames / min/maxProperties。
ValidationReport process_object_cluster(const JsonObject& schema_obj,
                                        const JsonValue& inst,
                                        EvalCtx& ctx, Annotations& ann);

ValidationReport eval_object_keywords(const JsonObject& schema_obj,
                                      const JsonValue& instance,
                                      EvalCtx& ctx,
                                      Annotations& ann) {

    // type first (short-circuit). Skip when validation vocab disabled.
    if (ctx.schema.disable_validation_vocab != 1) {
        for (auto& [k, v] : schema_obj) {
            if (k == "type") {
                if (!type_matches(v, instance))
                    return err_report(ValidationError::kTypeMismatch);
                break;
            }
        }
    }

    // detect array/object cluster keywords for joint processing at end
    bool has_array_cluster  = false;
    bool has_object_cluster = false;

    for (auto& [k, v] : schema_obj) {
        if (k == "type") continue;

        if (k == "$schema" || k == "$id" || k == "$comment" || k == "$anchor" ||
            k == "$dynamicAnchor" || k == "$defs" || k == "definitions" ||
            k == "title" || k == "description" || k == "default" ||
            k == "examples" || k == "readOnly" || k == "writeOnly" ||
            k == "deprecated" || k == "contentEncoding" || k == "contentMediaType" ||
            k == "contentSchema") continue;

        if (k == "$ref") {
            if (!v.is_string()) return err_report(ValidationError::kSlowSchemaInvalid);
            const JsonValue* target = slow_resolve_ref_uri(ctx.schema,
                                                           ctx.current_base,
                                                           v.as_string());
            if (!target) return err_report(ValidationError::kSlowSchemaInvalid);
            if (++ctx.depth > EvalCtx::kMaxDepth)
                return err_report(ValidationError::kSlowSchemaInvalid);
            Annotations sub_ann;
            // base switch in eval_with_ann via push_id_scope_for_node (reads
            // indexed node_base; avoids stacked $id resolve on $ref).
            auto r = eval_with_ann(*target, instance, ctx, sub_ann);
            --ctx.depth;
            if (!r.ok()) return r;
            merge_ann(ann, sub_ann);
            continue;
        }
        if (k == "$dynamicRef") {
            // JSON Schema 2020-12 §8.2.3.2 dynamic reference resolution:
            //   1. resolve v to target T like $ref.
            //   2. extract fragment name ("#name").
            //   3. if T declares $dynamicAnchor = name, dynamic mode:
            //      scan dynamic_scope bottom→top for first frame with
            //      $dynamicAnchor = name; use that schema as target.
            //      else use T.
            //   4. no fragment or no matching $dynamicAnchor: treat as $ref.
            if (!v.is_string()) return err_report(ValidationError::kSlowSchemaInvalid);
            const std::string& ref_str = v.as_string();
            const JsonValue* target = slow_resolve_ref_uri(ctx.schema,
                                                           ctx.current_base,
                                                           ref_str);
            if (!target) return err_report(ValidationError::kSlowSchemaInvalid);

            // extract fragment
            std::string_view rv(ref_str);
            std::string anchor;
            auto h = rv.find('#');
            if (h != std::string_view::npos) {
                anchor.assign(rv.data() + h + 1, rv.size() - h - 1);
            }
            // check target has $dynamicAnchor = anchor
            bool target_has_dyn = false;
            if (!anchor.empty() && target->is_object()) {
                for (auto& [tk, tv] : target->as_object()) {
                    if (tk == "$dynamicAnchor" && tv.is_string() &&
                        tv.as_string() == anchor) {
                        target_has_dyn = true;
                        break;
                    }
                }
            }
            if (target_has_dyn && !ctx.dynamic_scope.empty()) {
                // scan frames outer→inner; for each, lookup
                // `<frame.base>#<anchor>` in resources; if hit node
                // has $dynamicAnchor=anchor (not plain $anchor),
                // use as target; first win.
                for (auto& f : ctx.dynamic_scope) {
                    std::string key = f.base_uri;
                    key.push_back('#');
                    key.append(anchor);
                    auto it = ctx.schema.resources.find(key);
                    if (it == ctx.schema.resources.end()) continue;
                    const JsonValue* cand = it->second;
                    if (!cand || !cand->is_object()) continue;
                    bool is_dyn = false;
                    for (auto& [tk, tv] : cand->as_object()) {
                        if (tk == "$dynamicAnchor" && tv.is_string() &&
                            tv.as_string() == anchor) { is_dyn = true; break; }
                    }
                    if (is_dyn) { target = cand; break; }
                }
            }

            if (++ctx.depth > EvalCtx::kMaxDepth)
                return err_report(ValidationError::kSlowSchemaInvalid);
            Annotations sub_ann;
            auto r = eval_with_ann(*target, instance, ctx, sub_ann);
            --ctx.depth;
            if (!r.ok()) return r;
            merge_ann(ann, sub_ann);
            continue;
        }

        if (k == "allOf") {
            if (!v.is_array()) return err_report(ValidationError::kSlowSchemaInvalid);
            for (auto& sub : v.as_array()) {
                Annotations sub_ann;
                auto r = eval_with_ann(sub, instance, ctx, sub_ann);
                if (!r.ok()) return err_report(ValidationError::kAllOfFailed);
                merge_ann(ann, sub_ann);
            }
            continue;
        }
        if (k == "anyOf") {
            if (!v.is_array()) return err_report(ValidationError::kSlowSchemaInvalid);
            bool any = false;
            for (auto& sub : v.as_array()) {
                Annotations sub_ann;
                if (eval_with_ann(sub, instance, ctx, sub_ann).ok()) {
                    any = true;
                    merge_ann(ann, sub_ann);   // all successful branches contribute annotations
                }
            }
            if (!any) return err_report(ValidationError::kAnyOfFailed);
            continue;
        }
        if (k == "oneOf") {
            if (!v.is_array()) return err_report(ValidationError::kSlowSchemaInvalid);
            int match = 0;
            Annotations winner_ann;
            for (auto& sub : v.as_array()) {
                Annotations sub_ann;
                if (eval_with_ann(sub, instance, ctx, sub_ann).ok()) {
                    ++match;
                    if (match > 1) return err_report(ValidationError::kOneOfFailed);
                    winner_ann = std::move(sub_ann);
                }
            }
            if (match != 1) return err_report(ValidationError::kOneOfFailed);
            merge_ann(ann, winner_ann);
            continue;
        }
        if (k == "not") {
            // not never contributes annotations (spec: not collects none)
            Annotations dropped;
            if (eval_with_ann(v, instance, ctx, dropped).ok())
                return err_report(ValidationError::kNotFailed);
            continue;
        }
        if (k == "if") {
            Annotations cond_ann;
            bool if_ok = eval_with_ann(v, instance, ctx, cond_ann).ok();
            if (if_ok) merge_ann(ann, cond_ann);  // matching if branch contributes annotation
            const JsonValue* branch = nullptr;
            for (auto& [k2, v2] : schema_obj) {
                if (if_ok && k2 == "then") { branch = &v2; break; }
                if (!if_ok && k2 == "else") { branch = &v2; break; }
            }
            if (branch) {
                Annotations br_ann;
                auto r = eval_with_ann(*branch, instance, ctx, br_ann);
                if (!r.ok()) return err_report(ValidationError::kIfThenElseFailed);
                merge_ann(ann, br_ann);
            }
            continue;
        }
        if (k == "then" || k == "else") continue;

        if (k == "const") { auto r = handle_const(v, instance, ctx.schema); if (!r.ok()) return r; continue; }
        if (k == "enum")  { auto r = handle_enum(v, instance, ctx.schema);  if (!r.ok()) return r; continue; }

        if (k == "minimum" || k == "maximum" ||
            k == "exclusiveMinimum" || k == "exclusiveMaximum" ||
            k == "multipleOf") {
            auto r = handle_number_kw(k, v, instance, ctx.schema); if (!r.ok()) return r; continue;
        }
        if (k == "minLength" || k == "maxLength" || k == "pattern" || k == "format") {
            auto r = handle_string_kw(k, v, instance, ctx.schema); if (!r.ok()) return r; continue;
        }

        if (k == "items" || k == "prefixItems" || k == "contains" ||
            k == "minContains" || k == "maxContains" ||
            k == "minItems" || k == "maxItems" || k == "uniqueItems" ||
            k == "unevaluatedItems" || k == "additionalItems") {
            has_array_cluster = true;
            continue;
        }
        if (k == "properties" || k == "patternProperties" ||
            k == "additionalProperties" || k == "propertyNames" ||
            k == "required" || k == "dependentRequired" ||
            k == "dependentSchemas" || k == "minProperties" || k == "maxProperties" ||
            k == "unevaluatedProperties") {
            has_object_cluster = true;
            continue;
        }
        // unknown keyword: ignore as annotation-only
    }

    if (has_array_cluster && instance.is_array()) {
        auto r = process_array_cluster(schema_obj, instance, ctx, ann);
        if (!r.ok()) return r;
    }
    if (has_object_cluster && instance.is_object()) {
        auto r = process_object_cluster(schema_obj, instance, ctx, ann);
        if (!r.ok()) return r;
    }
    return ok_report();
}

// Lightweight valid-schema check when $ref targets metaschema
// avoids full metaschema validator (~5KB + self-reference).
// Covers common keyword type constraints for definition / remote ref cases.
bool builtin_is_valid_schema(const JsonValue& v);

bool valid_schema_obj(const JsonValue& v) {
    if (v.is_bool()) return true;
    if (!v.is_object()) return false;
    return builtin_is_valid_schema(v);
}

bool nonneg_integer(const JsonValue& v) {
    if (v.is_int()) return v.as_int() >= 0;
    if (v.is_double()) {
        double d = v.as_double();
        return std::isfinite(d) && std::floor(d) == d && d >= 0;
    }
    return false;
}

bool positive_number(const JsonValue& v) {
    if (v.is_int()) return v.as_int() > 0;
    if (v.is_double()) {
        double d = v.as_double();
        return std::isfinite(d) && d > 0;
    }
    return false;
}

bool builtin_is_valid_schema(const JsonValue& v) {
    if (v.is_bool()) return true;
    if (!v.is_object()) return false;
    for (auto& [k, sv] : v.as_object()) {
        if (k == "type") {
            if (sv.is_string()) continue;
            if (sv.is_array()) {
                for (auto& e : sv.as_array())
                    if (!e.is_string()) return false;
                continue;
            }
            return false;
        }
        if (k == "minLength" || k == "maxLength" || k == "minItems" || k == "maxItems" ||
            k == "minContains" || k == "maxContains" || k == "minProperties" ||
            k == "maxProperties") {
            if (!nonneg_integer(sv)) return false;
            continue;
        }
        if (k == "minimum" || k == "maximum" ||
            k == "exclusiveMinimum" || k == "exclusiveMaximum") {
            if (!sv.is_number()) return false;
            continue;
        }
        if (k == "multipleOf") { if (!positive_number(sv)) return false; continue; }
        if (k == "uniqueItems") { if (!sv.is_bool()) return false; continue; }
        if (k == "required") {
            if (!sv.is_array()) return false;
            for (auto& e : sv.as_array())
                if (!e.is_string()) return false;
            continue;
        }
        if (k == "pattern" || k == "format" || k == "$ref" || k == "$dynamicRef" ||
            k == "$id" || k == "$anchor" || k == "$dynamicAnchor" ||
            k == "$schema" || k == "$comment" || k == "title" ||
            k == "description" || k == "contentMediaType" || k == "contentEncoding") {
            if (!sv.is_string()) return false;
            continue;
        }
        if (k == "properties" || k == "patternProperties" || k == "$defs" ||
            k == "definitions" || k == "dependentSchemas") {
            if (!sv.is_object()) return false;
            for (auto& [pk, pv] : sv.as_object())
                if (!valid_schema_obj(pv)) return false;
            continue;
        }
        if (k == "items" || k == "contains" || k == "additionalProperties" ||
            k == "propertyNames" || k == "unevaluatedItems" ||
            k == "unevaluatedProperties" || k == "not" || k == "if" ||
            k == "then" || k == "else" || k == "additionalItems" ||
            k == "contentSchema") {
            // in 2020-12 "items" may also be a schema-array (legacy)
            if (sv.is_array()) {
                for (auto& e : sv.as_array())
                    if (!valid_schema_obj(e)) return false;
            } else if (!valid_schema_obj(sv)) return false;
            continue;
        }
        if (k == "prefixItems" || k == "allOf" || k == "anyOf" || k == "oneOf") {
            if (!sv.is_array() || sv.as_array().empty()) return false;
            for (auto& e : sv.as_array())
                if (!valid_schema_obj(e)) return false;
            continue;
        }
        if (k == "enum") {
            if (!sv.is_array()) return false;
            continue;
        }
        if (k == "dependentRequired") {
            if (!sv.is_object()) return false;
            for (auto& [dk, dv] : sv.as_object()) {
                if (!dv.is_array()) return false;
                for (auto& e : dv.as_array())
                    if (!e.is_string()) return false;
            }
            continue;
        }
        if (k == "const" || k == "default" || k == "examples" || k == "readOnly" ||
            k == "writeOnly" || k == "deprecated" || k == "$vocabulary") {
            // no constraint
            continue;
        }
        // other (custom) keywords: spec default annotation-only, allowed.
    }
    return true;
}

ValidationReport eval_with_ann(const JsonValue& schema_node, const JsonValue& instance,
                               EvalCtx& ctx, Annotations& ann) {
    // metaschema sentinel hit: use embedded lightweight schema validity checker.
    if (&schema_node == &metaschema_sentinel()) {
        return builtin_is_valid_schema(instance)
                   ? ok_report()
                   : err_report(ValidationError::kSlowSchemaInvalid);
    }
    if (schema_node.is_bool()) {
        return schema_node.as_bool() ? ok_report()
                                     : err_report(ValidationError::kTypeMismatch);
    }
    if (!schema_node.is_object()) return err_report(ValidationError::kSlowSchemaInvalid);

    // Switch to this subschema's base URI scope. Indexing stored post-$id base in
    // node_base; reuse it to avoid duplicate resolve.
    struct BaseGuard {
        EvalCtx& ctx; std::string saved;
        ~BaseGuard() { ctx.current_base = std::move(saved); }
    };
    BaseGuard guard{ctx, push_id_scope_for_node(schema_node, ctx)};

    // On entering a new schema resource ($id boundary), push dynamic_scope frame.
    // Cross-boundary when saved base != current_base; empty stack counts as a frame.
    struct DynGuard {
        EvalCtx& ctx; bool active;
        ~DynGuard() { if (active) ctx.dynamic_scope.pop_back(); }
    };
    DynGuard dyn_guard{ctx, false};
    if (ctx.dynamic_scope.empty() ||
        ctx.dynamic_scope.back().base_uri != ctx.current_base) {
        ctx.dynamic_scope.push_back({ctx.current_base});
        dyn_guard.active = true;
    }

    return eval_object_keywords(schema_node.as_object(), instance, ctx, ann);
}

ValidationReport eval(const JsonValue& schema_node, const JsonValue& instance, EvalCtx& ctx) {
    Annotations ann;
    return eval_with_ann(schema_node, instance, ctx, ann);
}

// -----------------------------------------------------------------------------
// Array cluster joint processing (items / prefixItems / contains / additionalItems /
//                                 min/maxItems / uniqueItems / min/maxContains / unevaluatedItems)
//
// Unlike per-keyword sequential handling:
//   * additionalItems needs items-as-array prefix length
//   * minContains / maxContains need contains hit count
//   * unevaluatedItems needs indices consumed by items/prefixItems (incl. via $ref/allOf/anyOf)
// -----------------------------------------------------------------------------
ValidationReport process_array_cluster(const JsonObject& schema_obj,
                                       const JsonValue& inst,
                                       EvalCtx& ctx, Annotations& ann) {
    const auto& arr = inst.as_array();

    const JsonValue* items_v        = nullptr;
    const JsonValue* prefix_v       = nullptr;
    const JsonValue* contains_v     = nullptr;
    const JsonValue* add_items_v    = nullptr;
    const JsonValue* min_items_v    = nullptr;
    const JsonValue* max_items_v    = nullptr;
    const JsonValue* uniq_v         = nullptr;
    const JsonValue* min_cont_v     = nullptr;
    const JsonValue* max_cont_v     = nullptr;
    const JsonValue* unev_items_v   = nullptr;

    for (auto& [k, v] : schema_obj) {
        if      (k == "items")              items_v      = &v;
        else if (k == "prefixItems")        prefix_v     = &v;
        else if (k == "contains")           contains_v   = &v;
        else if (k == "additionalItems")    add_items_v  = &v;
        else if (k == "minItems")           min_items_v  = &v;
        else if (k == "maxItems")           max_items_v  = &v;
        else if (k == "uniqueItems")        uniq_v       = &v;
        else if (k == "minContains")        min_cont_v   = &v;
        else if (k == "maxContains")        max_cont_v   = &v;
        else if (k == "unevaluatedItems")   unev_items_v = &v;
    }

    bool no_val = (ctx.schema.disable_validation_vocab == 1);
    if (!no_val) {
        if (min_items_v && min_items_v->is_int() &&
            arr.size() < static_cast<std::size_t>(min_items_v->as_int()))
            return err_report(ValidationError::kArrayTooShort);
        if (max_items_v && max_items_v->is_int() &&
            arr.size() > static_cast<std::size_t>(max_items_v->as_int()))
            return err_report(ValidationError::kArrayTooLong);
        if (uniq_v && uniq_v->is_bool() && uniq_v->as_bool()) {
            for (std::size_t i = 0; i < arr.size(); ++i)
                for (std::size_t j = i + 1; j < arr.size(); ++j)
                    if (json_eq(arr[i], arr[j]))
                        return err_report(ValidationError::kArrayNotUnique);
        }
    }

    // prefixItems / items-as-array: consume prefix
    std::size_t prefix_consumed = 0;
    if (prefix_v && prefix_v->is_array()) {
        const auto& pf = prefix_v->as_array();
        std::size_t n = std::min(arr.size(), pf.size());
        for (std::size_t i = 0; i < n; ++i) {
            Annotations sub_ann;
            auto r = eval_with_ann(pf[i], arr[i], ctx, sub_ann);
            if (!r.ok()) return r;
        }
        prefix_consumed = n;
    } else if (items_v && items_v->is_array()) {
        // Legacy draft-07 tuple form
        const auto& pf = items_v->as_array();
        std::size_t n = std::min(arr.size(), pf.size());
        for (std::size_t i = 0; i < n; ++i) {
            Annotations sub_ann;
            auto r = eval_with_ann(pf[i], arr[i], ctx, sub_ann);
            if (!r.ok()) return r;
        }
        prefix_consumed = n;
    }
    ann.prefix_items_seen = std::max(ann.prefix_items_seen, prefix_consumed);

    // items (single-schema form) applies to elements after prefix
    if (items_v && !items_v->is_array()) {
        if (items_v->is_bool()) {
            if (!items_v->as_bool() && prefix_consumed < arr.size())
                return err_report(ValidationError::kArrayContainsViolation);
        } else if (items_v->is_object()) {
            for (std::size_t i = prefix_consumed; i < arr.size(); ++i) {
                Annotations sub_ann;
                auto r = eval_with_ann(*items_v, arr[i], ctx, sub_ann);
                if (!r.ok()) return r;
            }
        }
        ann.all_items_seen = true;   // single-schema items means all indices covered
    }

    // additionalItems: paired with items-as-array
    if (add_items_v && items_v && items_v->is_array()) {
        for (std::size_t i = prefix_consumed; i < arr.size(); ++i) {
            if (add_items_v->is_bool()) {
                if (!add_items_v->as_bool())
                    return err_report(ValidationError::kArrayContainsViolation);
            } else if (add_items_v->is_object()) {
                Annotations sub_ann;
                auto r = eval_with_ann(*add_items_v, arr[i], ctx, sub_ann);
                if (!r.ok()) return r;
            }
        }
        ann.all_items_seen = true;
    }

    // contains
    if (contains_v) {
        std::size_t hits = 0;
        std::vector<std::size_t> hit_idx;
        for (std::size_t i = 0; i < arr.size(); ++i) {
            Annotations sub_ann;
            if (eval_with_ann(*contains_v, arr[i], ctx, sub_ann).ok()) {
                ++hits; hit_idx.push_back(i);
            }
        }
        long min_c = (!no_val && min_cont_v && min_cont_v->is_int()) ? min_cont_v->as_int() : 1;
        long max_c = (!no_val && max_cont_v && max_cont_v->is_int()) ? max_cont_v->as_int() : -1;
        if (static_cast<long>(hits) < min_c)
            return err_report(ValidationError::kArrayContainsViolation);
        if (max_c >= 0 && static_cast<long>(hits) > max_c)
            return err_report(ValidationError::kArrayContainsViolation);
        for (auto i : hit_idx) ann.contains_indices.push_back(i);
    }

    // unevaluatedItems: apply schema to all unevaluated indices
    if (unev_items_v) {
        auto is_evaluated = [&](std::size_t i)->bool {
            if (ann.all_items_seen) return true;
            if (i < ann.prefix_items_seen) return true;
            for (auto j : ann.contains_indices) if (j == i) return true;
            return false;
        };
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (is_evaluated(i)) continue;
            if (unev_items_v->is_bool()) {
                if (!unev_items_v->as_bool())
                    return err_report(ValidationError::kUnknownField);
            } else if (unev_items_v->is_object()) {
                Annotations sub_ann;
                auto r = eval_with_ann(*unev_items_v, arr[i], ctx, sub_ann);
                if (!r.ok()) return r;
            }
        }
        ann.all_items_seen = true;
    }
    return ok_report();
}

// -----------------------------------------------------------------------------
// Object cluster joint processing
// -----------------------------------------------------------------------------
ValidationReport process_object_cluster(const JsonObject& schema_obj,
                                        const JsonValue& inst,
                                        EvalCtx& ctx, Annotations& ann) {
    const auto& obj = inst.as_object();

    const JsonValue* props_v        = nullptr;
    const JsonValue* patp_v         = nullptr;
    const JsonValue* addp_v         = nullptr;
    const JsonValue* names_v        = nullptr;
    const JsonValue* req_v          = nullptr;
    const JsonValue* dreq_v         = nullptr;
    const JsonValue* dschemas_v     = nullptr;
    const JsonValue* min_p_v        = nullptr;
    const JsonValue* max_p_v        = nullptr;
    const JsonValue* unev_p_v       = nullptr;

    for (auto& [k, v] : schema_obj) {
        if      (k == "properties")            props_v    = &v;
        else if (k == "patternProperties")     patp_v     = &v;
        else if (k == "additionalProperties")  addp_v     = &v;
        else if (k == "propertyNames")         names_v    = &v;
        else if (k == "required")              req_v      = &v;
        else if (k == "dependentRequired")     dreq_v     = &v;
        else if (k == "dependentSchemas")      dschemas_v = &v;
        else if (k == "minProperties")         min_p_v    = &v;
        else if (k == "maxProperties")         max_p_v    = &v;
        else if (k == "unevaluatedProperties") unev_p_v   = &v;
    }

    bool no_val = (ctx.schema.disable_validation_vocab == 1);
    if (!no_val) {
        if (min_p_v && min_p_v->is_int() &&
            obj.size() < static_cast<std::size_t>(min_p_v->as_int()))
            return err_report(ValidationError::kArrayTooShort);
        if (max_p_v && max_p_v->is_int() &&
            obj.size() > static_cast<std::size_t>(max_p_v->as_int()))
            return err_report(ValidationError::kArrayTooLong);

        if (req_v && req_v->is_array()) {
            for (auto& rn : req_v->as_array()) {
                if (!rn.is_string()) continue;
                if (!inst.find(rn.as_string()))
                    return err_report(ValidationError::kMissingRequired);
            }
        }
    }

    auto add_evaluated = [&ann](const std::string& name) {
        ann.evaluated_props.push_back(name);
    };

    if (names_v) {
        for (auto& [ik, _] : obj) {
            JsonValue tmp = JsonValue::make_string(ik);
            Annotations sub_ann;
            auto r = eval_with_ann(*names_v, tmp, ctx, sub_ann);
            if (!r.ok()) return r;
        }
    }

    if (props_v && props_v->is_object()) {
        for (auto& [pk, sub] : props_v->as_object()) {
            const JsonValue* m = inst.find(pk);
            if (!m) continue;
            Annotations sub_ann;
            auto r = eval_with_ann(sub, *m, ctx, sub_ann);
            if (!r.ok()) return r;
            add_evaluated(pk);
        }
    }
    if (patp_v && patp_v->is_object()) {
        for (auto& [ik, iv] : obj) {
            for (auto& [pat, sub] : patp_v->as_object()) {
                if (regex_search_inline(pat, ik)) {
                    Annotations sub_ann;
                    auto r = eval_with_ann(sub, iv, ctx, sub_ann);
                    if (!r.ok()) return r;
                    add_evaluated(ik);
                }
            }
        }
    }
    if (addp_v) {
        auto is_handled = [&](std::string_view name)->bool {
            if (props_v && props_v->is_object()) {
                for (auto& [pk, _] : props_v->as_object())
                    if (pk == name) return true;
            }
            if (patp_v && patp_v->is_object()) {
                for (auto& [pat, _] : patp_v->as_object())
                    if (regex_search_inline(pat, name)) return true;
            }
            return false;
        };
        for (auto& [ik, iv] : obj) {
            if (is_handled(ik)) continue;
            if (addp_v->is_bool()) {
                if (!addp_v->as_bool()) return err_report(ValidationError::kUnknownField);
            } else if (addp_v->is_object()) {
                Annotations sub_ann;
                auto r = eval_with_ann(*addp_v, iv, ctx, sub_ann);
                if (!r.ok()) return r;
            }
            add_evaluated(ik);
        }
    }
    if (!no_val && dreq_v && dreq_v->is_object()) {
        for (auto& [trig, deps] : dreq_v->as_object()) {
            if (!inst.find(trig)) continue;
            if (!deps.is_array()) continue;
            for (auto& d : deps.as_array()) {
                if (!d.is_string()) continue;
                if (!inst.find(d.as_string()))
                    return err_report(ValidationError::kDependentRequired);
            }
        }
    }
    if (dschemas_v && dschemas_v->is_object()) {
        for (auto& [trig, sub] : dschemas_v->as_object()) {
            if (!inst.find(trig)) continue;
            Annotations sub_ann;
            auto r = eval_with_ann(sub, inst, ctx, sub_ann);
            if (!r.ok()) return r;
            merge_ann(ann, sub_ann);   // dependentSchemas contributes annotations
        }
    }
    if (unev_p_v) {
        // set: properties marked by this cluster or outer ($ref / allOf / ...)
        auto is_evaluated = [&](std::string_view name)->bool {
            for (auto& p : ann.evaluated_props) if (p == name) return true;
            return false;
        };
        for (auto& [ik, iv] : obj) {
            if (is_evaluated(ik)) continue;
            if (unev_p_v->is_bool()) {
                if (!unev_p_v->as_bool()) return err_report(ValidationError::kUnknownField);
            } else if (unev_p_v->is_object()) {
                Annotations sub_ann;
                auto r = eval_with_ann(*unev_p_v, iv, ctx, sub_ann);
                if (!r.ok()) return r;
            }
            add_evaluated(ik);
        }
    }
    return ok_report();
}

}  // namespace

ValidationReport validate_slow_path(std::string_view json_instance,
                                    const SlowSchema& schema) noexcept {
    auto parsed = parse_json(json_instance);
    if (!parsed.ok) {
        ValidationReport r;
        r.code   = ValidationError::kInvalidJson;
        r.offset = static_cast<std::uint32_t>(parsed.error_offset);
        return r;
    }
    // lazily compute validation vocabulary state (after all remotes injected).
    if (schema.disable_validation_vocab < 0) {
        schema.disable_validation_vocab = check_validation_vocab_disabled(schema) ? 1 : 0;
    }
    EvalCtx ctx{schema, 0, schema.primary_base};
    return eval(schema.root, parsed.value, ctx);
}

}  // namespace iris
