// =============================================================================
// iris/schema.hpp
//
// Compile-time / load-time schema descriptor.
//
// Everything is SoA, POD, copyable:
//
//   types[i]          : allowed type mask for field i (bitwise DFA expected mask)
//   required_mask     : required-field bit mask (bit i means slot i required)
//   max_string_len[i] : max string length (0 = none)
//   min_int[i]/max_int[i] : integer bounds
//
// Simpler schemas yield tighter bitwise DFAs. Complex schemas are blocked by
// Inspector at load time and routed to the slow path.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "iris/common.hpp"
#include "iris/perfect_hash.hpp"

namespace iris {

struct CompiledSchema;  // forward: CompiledSchema is self-recursive

// Type mask: low 8 bits align with TokenKind
enum TypeBit : std::uint8_t {
    kTypeNone    = 0,
    kTypeObject  = 1u << 0,
    kTypeArray   = 1u << 1,
    kTypeString  = 1u << 2,
    kTypeNumber  = 1u << 3,
    kTypeInteger = 1u << 4,
    kTypeBoolean = 1u << 5,
    kTypeNull    = 1u << 6,
};

using TypeMask = std::uint8_t;

[[nodiscard]] inline TypeMask type_from_keyword(std::string_view kw) noexcept {
    if (kw == "object")  return kTypeObject;
    if (kw == "array")   return kTypeArray;
    if (kw == "string")  return kTypeString;
    if (kw == "number")  return kTypeNumber | kTypeInteger;
    if (kw == "integer") return kTypeInteger;
    if (kw == "boolean") return kTypeBoolean;
    if (kw == "null")    return kTypeNull;
    return kTypeNone;
}

// Short-key direct lookup table
//
// When all schema field names are <= 8 bytes (most real schemas):
//   - Pack each name into one uint64_t (little-endian, zero-pad high bits)
//   - Lookup: one 8-byte unaligned load + length mask + linear compare
//
// Saves ~30 cycles per key vs fmix64 + mod.
//
// Caller must ensure (key_ptr + 8) is readable — iris_validate appends 16 zero bytes.
struct ShortKeyTable {
    static constexpr std::size_t kMaxKeys = 16;
    std::uint64_t bits[kMaxKeys] = {};
    std::uint8_t  lens[kMaxKeys] = {};
    std::int8_t   slot[kMaxKeys] = {};
    std::uint8_t  count = 0;
};

// Root type of CompiledSchema: determines validate() entry dispatch
enum class SchemaKind : std::uint8_t {
    kAlwaysValid   = 0,   // schema == true or {}
    kAlwaysInvalid = 1,   // schema == false
    kObjectRoot    = 2,   // top-level type=object, recursive validate_object
    kValueRoot     = 3,   // top-level string/number/integer/array/bool/null
};

// Compiled artifact: recursive object/array schema.
//
// SchemaKind controls root behavior;
// for kObjectRoot: types[i] is allowed type mask for property i;
// for kValueRoot: only slot 0 stores root type+constraints (synthetic single-value slot);
// for kAlways*: all fields empty, validate returns immediately.
//
// Nested fields:
//   - field type includes object -> nested_object[slot] holds child schema
//   - field type includes array  -> array_item_type[slot] is item TypeMask;
//                                   if item is object, array_item_nested[slot] recurses
//
// Three parallel vectors maintain SoA style; non-nested slots use nullptr / kTypeNone.
struct CompiledSchema {
    SchemaKind                 kind = SchemaKind::kObjectRoot;
    PerfectHashTable           field_index;
    ShortKeyTable              short_keys;
    std::vector<TypeMask>      types;
    std::vector<std::uint32_t> min_string_len;
    std::vector<std::uint32_t> max_string_len;
    std::vector<std::int64_t>  min_int;
    std::vector<std::int64_t>  max_int;
    // Float min/max parallel to min_int/max_int; enabled for number fields only.
    // Default -DBL_MAX / +DBL_MAX means unconstrained; avoid infinity() because -ffast-math
    // folds numeric_limits::infinity() to 0 (assumes finite-math-only).
    std::vector<double>        min_dbl;
    std::vector<double>        max_dbl;
    std::vector<std::string>   field_names;
    std::vector<std::unique_ptr<CompiledSchema>> nested_object;
    std::vector<TypeMask>                        array_item_type;
    std::vector<std::unique_ptr<CompiledSchema>> array_item_nested;
    std::uint64_t              required_mask = 0;
    bool                       additional_properties = true;

    [[nodiscard]] std::size_t field_count() const noexcept { return types.size(); }
};

// Restricted schema field descriptor for DSL (no nesting; use compile_schema_from_json)
struct FieldSpec {
    std::string_view name;
    TypeMask         type           = kTypeNone;
    bool             required       = false;
    std::uint32_t    min_string_len = 0;
    std::uint32_t    max_string_len = 0;
    std::int64_t     min_int        = INT64_MIN;
    std::int64_t     max_int        = INT64_MAX;
};

// Convenience: build CompiledSchema from FieldSpec array.
// On failure ok=false, diagnostic explains (e.g. perfect hash construction failed).
struct SchemaBuildResult {
    bool             ok = false;
    CompiledSchema   schema;
    std::string      diagnostic;
};

SchemaBuildResult compile_schema(std::span<const FieldSpec> fields,
                                 bool additional_properties = false);

// Compile directly from JSON Schema document text.
//
// Currently covers draft-2020-12 core subset:
//   - type: single / array
//   - properties / required / additionalProperties
//   - minimum / maximum (integer)
//   - minLength / maxLength
//
// Unsupported keywords (pattern / $ref / allOf etc.) are vetoed by Inspector
// for slow path; current stub rejects outright.
SchemaBuildResult compile_schema_from_json(std::string_view schema_json);

}  // namespace iris
