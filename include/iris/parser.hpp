// =============================================================================
// iris/parser.hpp
//
// Fused Parse & Validate engine (Phase 2 Fast Path)
//
// Design principles (from whitepaper):
//
//   - Zero-DOM: no heap JSON tree; scan raw buffer only
//   - Zero Allocation: all state on stack / caller-provided arena
//   - Bitwise DFA: type bit masks instead of if/else chains
//   - Perfect Hash: O(1) field name -> slot dispatch
//
// Current phase limitations (documented for later upgrades):
//
//   - Single-level object schema validation only
//   - Numbers as int64 / double only; no full IEEE-754 precision recovery
//   - Strings: no \u escape decoding (length counted in bytes)
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "iris/common.hpp"
#include "iris/schema.hpp"

namespace iris {

enum class ValidationError : std::uint8_t {
    kOk = 0,
    kInvalidJson,
    kUnexpectedToken,
    kUnknownField,
    kMissingRequired,
    kTypeMismatch,
    kStringTooShort,
    kStringTooLong,
    kIntOutOfRange,
    kDuplicateField,
    // Slow path related
    kNotImplemented,     // schema keyword not supported (slow path not implemented)
    kConstMismatch,
    kEnumMismatch,
    kMultipleOf,
    kPatternMismatch,
    kArrayTooShort,
    kArrayTooLong,
    kArrayNotUnique,
    kArrayContainsViolation,
    kAllOfFailed,
    kAnyOfFailed,
    kOneOfFailed,
    kNotFailed,
    kIfThenElseFailed,
    kDependentRequired,
    kSlowSchemaInvalid,  // schema JSON itself is invalid
};

const char* validation_error_name(ValidationError e) noexcept;

struct ValidationReport {
    ValidationError code = ValidationError::kOk;
    // Error offset: byte offset in raw buffer
    std::uint32_t   offset = 0;
    // Error field (if identified): slot in schema field list; -1 if not tied to a field
    std::int32_t    field_slot = -1;
    // Seen-field bit mask (debug)
    std::uint64_t   seen_mask = 0;

    [[nodiscard]] bool ok() const noexcept { return code == ValidationError::kOk; }
};

// Main entry: fused validation — returns no DOM, allocates nothing
//
// data: raw JSON bytes (no NUL terminator required)
// size: byte length
// schema: compiled CompiledSchema
ValidationReport validate(const std::uint8_t* IRIS_RESTRICT data,
                          std::size_t size,
                          const CompiledSchema& schema) noexcept;

[[nodiscard]] IRIS_FORCE_INLINE ValidationReport
validate(std::string_view view, const CompiledSchema& schema) noexcept {
    return validate(reinterpret_cast<const std::uint8_t*>(view.data()),
                    view.size(), schema);
}

}  // namespace iris
