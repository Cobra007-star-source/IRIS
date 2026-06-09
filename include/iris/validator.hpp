// =============================================================================
// iris/validator.hpp
//
// Public API: dual-engine entry.
//
// Usage:
//
//   iris::FieldSpec fields[] = { ... };
//   auto built = iris::compile_schema(fields);
//   if (!built.ok) { ... }
//
//   iris::Validator v(std::move(built.schema));
//   auto r = v.validate(R"({"name":"Iris","age":3})");
//   if (!r.ok()) { ... }
// =============================================================================
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "iris/common.hpp"
#include "iris/inspector.hpp"
#include "iris/jit.hpp"
#include "iris/parser.hpp"
#include "iris/schema.hpp"
#include "iris/slow_path.hpp"
#include "iris/slow_schema.hpp"

namespace iris {

enum class EnginePath : std::uint8_t {
    kFastInterpret = 0,
    kFastJit       = 1,
    kSlowFallback  = 2,
    kAlwaysValid   = 3,
    kAlwaysInvalid = 4,
};

const char* engine_path_name(EnginePath p) noexcept;

struct ValidatorBuild {
    bool         ok = false;
    std::string  diagnostic;
};

class Validator : public NonCopyable {
public:
    // Classic entry: accepts CompiledSchema that already passed Fast Path. Kept for compatibility.
    explicit Validator(CompiledSchema schema,
                       SchemaInspectionInput inspection = {});

    // Recommended entry: schema JSON string with automatic fast/slow routing.
    //   1. Try compile_schema_from_json (Fast Path, CompiledSchema/SoA)
    //   2. On Fast Path rejection (unsupported keyword), fallback to
    //      compile_slow_schema (Slow Path, JsonValue AST + RE2 cache + $ref table)
    //   3. If both fail, build.ok=false with diagnostic
    [[nodiscard]] static Validator from_schema_json(std::string_view schema_json,
                                                    ValidatorBuild& out_build);

    Validator(Validator&&) noexcept = default;
    Validator& operator=(Validator&&) noexcept = default;

    [[nodiscard]] ValidationReport validate(std::string_view json) const noexcept;

    [[nodiscard]] EnginePath           path()   const noexcept { return path_; }
    [[nodiscard]] const CompiledSchema& schema() const noexcept { return fast_schema_; }
    [[nodiscard]] const InspectionResult& inspection() const noexcept { return inspection_; }
    [[nodiscard]] bool has_fast_path() const noexcept { return has_fast_; }
    [[nodiscard]] bool has_slow_path() const noexcept { return slow_schema_ != nullptr; }

    // Register an external document for Slow engine $ref resolution.
    // Only when has_slow_path(); if only Fast Path, switches back to Slow (re-compile).
    // Returns whether registration succeeded.
    bool add_remote_document(std::string uri, std::string_view json) noexcept;

private:
    Validator() = default;

    CompiledSchema                fast_schema_;
    bool                          has_fast_ = false;
    std::unique_ptr<SlowSchema>   slow_schema_;
    InspectionResult              inspection_{};
    EnginePath                    path_     = EnginePath::kFastInterpret;
    jit::JitValidatorFn           jit_fn_   = nullptr;
    std::unique_ptr<jit::ExecutableBuffer> jit_buffer_;
};

}  // namespace iris
