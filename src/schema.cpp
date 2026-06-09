// =============================================================================
// schema.cpp
// Compiler from FieldSpec[] to CompiledSchema (perfect hash + SoA layout)
// =============================================================================
#include "iris/schema.hpp"

#include <cfloat>
#include <climits>
#include <limits>
#include <sstream>
#include <vector>

#include "iris/json_reader.hpp"

namespace iris {

SchemaBuildResult compile_schema(std::span<const FieldSpec> fields,
                                 bool additional_properties) {
    SchemaBuildResult r;

    if (fields.size() > 64) {
        r.diagnostic = "schema field count > 64 (required_mask is uint64)";
        return r;
    }

    std::vector<std::string_view> keys;
    keys.reserve(fields.size());
    for (auto& f : fields) keys.push_back(f.name);

    auto ph = build_perfect_hash(keys);
    if (!ph.success) {
        r.diagnostic = "perfect-hash build failed: " + ph.diagnostic;
        return r;
    }

    r.schema.kind                  = SchemaKind::kObjectRoot;
    r.schema.field_index           = std::move(ph.table);
    r.schema.additional_properties = additional_properties;
    const std::size_t N = fields.size();
    r.schema.types.assign(N, kTypeNone);
    r.schema.min_string_len.assign(N, 0);
    r.schema.max_string_len.assign(N, 0);
    r.schema.min_int.assign(N, INT64_MIN);
    r.schema.max_int.assign(N, INT64_MAX);
    // -ffast-math assumes finite-math-only; numeric_limits::infinity() folds to 0.
    // Use ±DBL_MAX for equivalent "unconstrained" semantics on valid JSON numbers.
    r.schema.min_dbl.assign(N, -DBL_MAX);
    r.schema.max_dbl.assign(N,  DBL_MAX);
    r.schema.field_names.assign(N, std::string{});
    r.schema.nested_object.resize(N);
    r.schema.array_item_type.assign(N, kTypeNone);
    r.schema.array_item_nested.resize(N);

    for (std::size_t i = 0; i < N; ++i) {
        std::string_view key = fields[i].name;
        std::int32_t slot = r.schema.field_index.lookup(key);
        if (slot < 0) {
            r.diagnostic = "internal: PH cannot find key just inserted";
            return r;
        }
        r.schema.types[slot]          = fields[i].type;
        r.schema.min_string_len[slot] = fields[i].min_string_len;
        r.schema.max_string_len[slot] = fields[i].max_string_len;
        r.schema.min_int[slot]        = fields[i].min_int;
        r.schema.max_int[slot]        = fields[i].max_int;
        // Mirror int bounds to double so fast path uses one comparison path for numbers;
        // INT64_MIN/MAX map to ±inf for "unconstrained"; avoid 1<<63 float precision loss.
        r.schema.min_dbl[slot] = (fields[i].min_int == std::numeric_limits<std::int64_t>::min())
                                   ? -DBL_MAX
                                   : static_cast<double>(fields[i].min_int);
        r.schema.max_dbl[slot] = (fields[i].max_int == std::numeric_limits<std::int64_t>::max())
                                   ?  DBL_MAX
                                   : static_cast<double>(fields[i].max_int);
        r.schema.field_names[slot]    = std::string(key);
        if (fields[i].required) {
            r.schema.required_mask |= (1ULL << slot);
        }
    }

    // Short-key direct table: only when all keys len ≤ 8 and field count ≤ kMaxKeys
    bool all_short = (N <= ShortKeyTable::kMaxKeys);
    if (all_short) {
        for (auto& f : fields) {
            if (f.name.size() > 8) { all_short = false; break; }
        }
    }
    if (all_short) {
        ShortKeyTable& tab = r.schema.short_keys;
        tab.count = static_cast<std::uint8_t>(N);
        for (std::size_t i = 0; i < N; ++i) {
            std::string_view key = fields[i].name;
            std::uint64_t bits = 0;
            for (std::size_t b = 0; b < key.size(); ++b) {
                bits |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(key[b])) << (b * 8);
            }
            tab.bits[i] = bits;
            tab.lens[i] = static_cast<std::uint8_t>(key.size());
            tab.slot[i] = static_cast<std::int8_t>(r.schema.field_index.lookup(key));
        }
    }

    r.ok = true;
    return r;
}

// =============================================================================
// JSON Schema document parser (recursive; properties / items nesting)
// =============================================================================

namespace {

TypeMask extract_type(const JsonValue* tv) noexcept {
    if (!tv) return kTypeNone;
    if (tv->is_string()) return type_from_keyword(tv->as_string());
    if (tv->is_array()) {
        TypeMask acc = 0;
        for (auto& v : tv->as_array()) {
            if (v.is_string()) acc |= type_from_keyword(v.as_string());
        }
        return acc;
    }
    return kTypeNone;
}

bool is_required_field(const JsonValue* req, std::string_view name) {
    if (!req || !req->is_array()) return false;
    for (auto& v : req->as_array()) {
        if (v.is_string() && v.as_string() == name) return true;
    }
    return false;
}

struct CompileError {
    std::string msg;
};

// Keywords unsupported by fast path; any one fails compile → slow path.
constexpr std::string_view kUnsupported[] = {
    "$ref", "$dynamicRef", "$anchor", "$dynamicAnchor",
    "allOf", "anyOf", "oneOf", "not",
    "if", "then", "else",
    "pattern", "patternProperties", "propertyNames",
    "format",
    "const", "enum",
    "multipleOf",
    "exclusiveMinimum", "exclusiveMaximum",
    "minItems", "maxItems", "uniqueItems",
    "minProperties", "maxProperties",
    "contains", "minContains", "maxContains",
    "prefixItems", "unevaluatedItems", "unevaluatedProperties",
    "dependentRequired", "dependentSchemas", "dependencies",
    "contentMediaType", "contentEncoding", "contentSchema",
};

bool keyword_unsupported(std::string_view k) noexcept {
    for (auto& s : kUnsupported) if (k == s) return true;
    return false;
}

// Recursively compile a JSON Schema object into CompiledSchema.
// Throws CompileError on failure (compile time only; hot path does not throw).
CompiledSchema compile_root(const JsonValue& v);
CompiledSchema compile_object_schema(const JsonValue& v);

// Extract single-value constraints (type + simple scalars) from generic schema.
// Throws CompileError on failure.
struct ValueConstraintBuild {
    TypeMask                        allowed         = kTypeNone;
    std::uint32_t                   min_string_len  = 0;
    std::uint32_t                   max_string_len  = 0;
    std::int64_t                    min_int         = INT64_MIN;
    std::int64_t                    max_int         = INT64_MAX;
    double                          min_dbl         = -DBL_MAX;
    double                          max_dbl         =  DBL_MAX;
    TypeMask                        item_type       = kTypeNone;
    std::unique_ptr<CompiledSchema> item_nested;
    std::unique_ptr<CompiledSchema> obj_nested;
};

ValueConstraintBuild compile_value_schema(const JsonValue& v) {
    if (!v.is_object()) throw CompileError{"schema fragment must be an object"};
    ValueConstraintBuild r;

    const JsonValue* type_v  = nullptr;
    const JsonValue* items_v = nullptr;
    for (auto& [k, x] : v.as_object()) {
        if      (k == "type")      type_v  = &x;
        else if (k == "items")     items_v = &x;
        else if (k == "minLength" && x.is_int())  r.min_string_len = static_cast<std::uint32_t>(x.as_int());
        else if (k == "maxLength" && x.is_int())  r.max_string_len = static_cast<std::uint32_t>(x.as_int());
        else if (k == "minimum"   && x.is_number()) { r.min_int = x.as_int(); r.min_dbl = x.as_double(); }
        else if (k == "maximum"   && x.is_number()) { r.max_int = x.as_int(); r.max_dbl = x.as_double(); }
        else if (k == "properties" || k == "required" || k == "additionalProperties") {
            // handled by compile_object_schema; skip here
        }
        else if (keyword_unsupported(k)) {
            throw CompileError{"unsupported keyword: " + std::string(k)};
        }
        // ignore other allowed metadata (description / $schema / $id / examples / default)
    }
    r.allowed = extract_type(type_v);
    if (r.allowed == kTypeNone) {
        // no type: any type; fast path uses all-ones mask for "any".
        r.allowed = 0xFFu;
    }

    if (r.allowed & kTypeObject) {
        // any keyword constraining object shape triggers obj_nested:
        //   - properties (field types/constraints)
        //   - additionalProperties:false (reject unknown fields)
        //   - required (mandatory fields; ghost slots)
        const JsonValue* props_v = nullptr;
        const JsonValue* addp_v  = nullptr;
        const JsonValue* req_v   = nullptr;
        for (auto& [k, x] : v.as_object()) {
            if      (k == "properties")           props_v = &x;
            else if (k == "additionalProperties") addp_v  = &x;
            else if (k == "required")             req_v   = &x;
        }
        bool addp_false = (addp_v && addp_v->is_bool() && !addp_v->as_bool());
        bool has_req    = (req_v && req_v->is_array() && !req_v->as_array().empty());
        if (props_v || addp_false || has_req) {
            r.obj_nested = std::make_unique<CompiledSchema>(compile_object_schema(v));
        }
    }
    if (r.allowed & kTypeArray) {
        if (items_v) {
            if (!items_v->is_object()) {
                // items: true / false / array of schemas → slow path for now
                throw CompileError{"items must be a single object schema in fast path"};
            }
            // reject nested arrays — fast path CompiledSchema
            // only carries object-nested item slot; cannot express another array level;
            // fall through to slow path, which supports arbitrary depth.
            for (auto& [ik, iv] : items_v->as_object()) {
                if (ik == "items" || ik == "prefixItems")
                    throw CompileError{"nested array items not supported in fast path"};
            }
            auto inner = compile_value_schema(*items_v);
            r.item_type = inner.allowed;
            // if item is object, transfer its obj_nested
            r.item_nested = std::move(inner.obj_nested);
        }
    }
    return r;
}

// Legacy API kept for compile_object_schema internals
struct ItemBuild {
    TypeMask                        item_type = kTypeNone;
    std::unique_ptr<CompiledSchema> item_nested;
};
ItemBuild compile_item_schema(const JsonValue& v) {
    auto b = compile_value_schema(v);
    return {b.allowed, std::move(b.item_nested ? b.item_nested : b.obj_nested)};
}

CompiledSchema compile_object_schema(const JsonValue& v) {
    if (!v.is_object()) throw CompileError{"schema must be an object"};
    const JsonObject& root = v.as_object();

    const JsonValue* type_v  = nullptr;
    const JsonValue* props_v = nullptr;
    const JsonValue* req_v   = nullptr;
    const JsonValue* addp_v  = nullptr;
    for (auto& [k, x] : root) {
        if      (k == "type")                 type_v  = &x;
        else if (k == "properties")           props_v = &x;
        else if (k == "required")             req_v   = &x;
        else if (k == "additionalProperties") addp_v  = &x;
        else if (keyword_unsupported(k))      throw CompileError{"unsupported keyword: " + std::string(k)};
    }

    if (type_v) {
        TypeMask tm = extract_type(type_v);
        if (!(tm & kTypeObject)) throw CompileError{"object schema must have type containing 'object'"};
    }
    bool additional_props = true;
    if (addp_v) {
        if (!addp_v->is_bool()) throw CompileError{"additionalProperties must be boolean"};
        additional_props = addp_v->as_bool();
    }

    // Collect field names from properties plus required-only ghost slots
    std::vector<std::string> name_buf;
    std::vector<ValueConstraintBuild> vc_buf;
    std::vector<const JsonValue*> sub_buf;

    // Reject keys needing JSON escape. Fast path perfect hash hashes raw bytes
    // (including escape backslashes), so \n / \" / \\ in names would mismatch;
    // reject immediately so schema uses slow path.
    auto needs_escape = [](std::string_view s)->bool {
        for (unsigned char c : s) {
            if (c < 0x20 || c == '"' || c == '\\') return true;
        }
        return false;
    };

    if (props_v && props_v->is_object()) {
        for (auto& [name, sub] : props_v->as_object()) {
            if (needs_escape(name))
                throw CompileError{"property name needs JSON escaping: '" + name + "'"};
            if (!sub.is_object()) throw CompileError{"property '" + name + "' must be an object"};
            name_buf.push_back(name);
            vc_buf.push_back(compile_value_schema(sub));
            sub_buf.push_back(&sub);
        }
    }
    // ghost slots: required but not in properties
    if (req_v && req_v->is_array()) {
        for (auto& rv : req_v->as_array()) {
            if (!rv.is_string()) continue;
            const auto& rn = rv.as_string();
            if (needs_escape(rn))
                throw CompileError{"required name needs JSON escaping: '" + rn + "'"};
            bool already = false;
            for (auto& n : name_buf) if (n == rn) { already = true; break; }
            if (already) continue;
            name_buf.push_back(rn);
            ValueConstraintBuild ghost;
            ghost.allowed = 0xFFu;
            vc_buf.push_back(std::move(ghost));
            sub_buf.push_back(nullptr);
        }
    }

    std::vector<FieldSpec> field_storage;
    field_storage.reserve(name_buf.size());
    for (std::size_t i = 0; i < name_buf.size(); ++i) {
        FieldSpec spec{};
        spec.name            = name_buf[i];
        spec.required        = is_required_field(req_v, name_buf[i]);
        spec.type            = vc_buf[i].allowed;
        spec.min_string_len  = vc_buf[i].min_string_len;
        spec.max_string_len  = vc_buf[i].max_string_len;
        spec.min_int         = vc_buf[i].min_int;
        spec.max_int         = vc_buf[i].max_int;
        field_storage.push_back(spec);
    }

    auto built = compile_schema(field_storage, additional_props);
    if (!built.ok) throw CompileError{"compile_schema: " + built.diagnostic};
    built.schema.kind = SchemaKind::kObjectRoot;

    CompiledSchema& cs = built.schema;
    for (std::size_t i = 0; i < name_buf.size(); ++i) {
        std::int32_t slot = cs.field_index.lookup(name_buf[i]);
        if (slot < 0) throw CompileError{"internal: PH cannot find key"};
        cs.min_dbl[slot] = vc_buf[i].min_dbl;
        cs.max_dbl[slot] = vc_buf[i].max_dbl;
        cs.field_names[slot] = name_buf[i];

        if (vc_buf[i].allowed & kTypeObject) {
            // ghost slot has no JsonValue node; recurse only declared properties
            if (sub_buf[i]) {
                // reuse vc's already-compiled obj_nested
                cs.nested_object[slot] = std::move(vc_buf[i].obj_nested);
            }
        }
        if (vc_buf[i].allowed & kTypeArray) {
            cs.array_item_type[slot]   = vc_buf[i].item_type;
            cs.array_item_nested[slot] = std::move(vc_buf[i].item_nested);
        }
    }
    return std::move(built.schema);
}

// compile_root: dispatch by top-level shape
//   - true            → kAlwaysValid
//   - false           → kAlwaysInvalid
//   - {} empty object  → kAlwaysValid
//   - {"type":"object", ...} (single type) → kObjectRoot (strict; non-object fails)
//   - other            → kValueRoot (properties/items/required vacuous on mismatch)
CompiledSchema compile_root(const JsonValue& v) {
    if (v.is_bool()) {
        CompiledSchema s;
        s.kind = v.as_bool() ? SchemaKind::kAlwaysValid : SchemaKind::kAlwaysInvalid;
        return s;
    }
    if (!v.is_object()) throw CompileError{"top-level schema must be a boolean or object"};
    const JsonObject& root = v.as_object();
    if (root.empty()) {
        CompiledSchema s; s.kind = SchemaKind::kAlwaysValid; return s;
    }
    for (auto& [k, x] : root) {
        if (keyword_unsupported(k)) throw CompileError{"unsupported keyword: " + std::string(k)};
        if (k == "additionalProperties" && !x.is_bool()) {
            throw CompileError{"additionalProperties as schema (non-bool) not supported"};
        }
        if (k == "items" && !x.is_object()) {
            // items: bool / items: [...] (tuple) → slow path for now
            throw CompileError{"items as non-object (bool or array) not supported"};
        }
    }

    // extract type; kObjectRoot strict path only for sole "object" type
    const JsonValue* type_v = nullptr;
    for (auto& [k, x] : root) if (k == "type") { type_v = &x; break; }
    bool type_is_only_object = (type_v && type_v->is_string() && type_v->as_string() == "object");

    if (type_is_only_object) {
        return compile_object_schema(v);
    }

    // kValueRoot: build 1-slot synthetic schema
    ValueConstraintBuild vc = compile_value_schema(v);
    CompiledSchema s;
    s.kind                  = SchemaKind::kValueRoot;
    s.additional_properties = true;
    s.types.assign(1, vc.allowed);
    s.min_string_len.assign(1, vc.min_string_len);
    s.max_string_len.assign(1, vc.max_string_len);
    s.min_int.assign(1, vc.min_int);
    s.max_int.assign(1, vc.max_int);
    s.min_dbl.assign(1, vc.min_dbl);
    s.max_dbl.assign(1, vc.max_dbl);
    s.field_names.assign(1, std::string{"@root"});
    s.nested_object.resize(1);
    s.array_item_type.assign(1, vc.item_type);
    s.array_item_nested.resize(1);
    s.nested_object[0]     = std::move(vc.obj_nested);
    s.array_item_nested[0] = std::move(vc.item_nested);
    return s;
}

}  // namespace

SchemaBuildResult compile_schema_from_json(std::string_view schema_json) {
    SchemaBuildResult r;
    auto p = parse_json(schema_json);
    if (!p.ok) {
        std::ostringstream oss;
        oss << "json parse error @" << p.error_offset << ": " << p.diagnostic;
        r.diagnostic = oss.str();
        return r;
    }
    try {
        r.schema = compile_root(p.value);
        r.ok = true;
    } catch (const CompileError& e) {
        r.ok = false;
        r.diagnostic = e.msg;
    }
    return r;
}

}  // namespace iris
