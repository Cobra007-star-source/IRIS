// =============================================================================
// validator.cpp
// Dual-engine routing: fast path (SIMD) and slow path (AST interpreter)
// =============================================================================
#include "iris/validator.hpp"

namespace iris {

const char* engine_path_name(EnginePath p) noexcept {
    switch (p) {
        case EnginePath::kFastInterpret: return "fast-interpret";
        case EnginePath::kFastJit:       return "fast-jit";
        case EnginePath::kSlowFallback:  return "slow-fallback";
        case EnginePath::kAlwaysValid:   return "always-valid";
        case EnginePath::kAlwaysInvalid: return "always-invalid";
    }
    return "unknown";
}

Validator::Validator(CompiledSchema schema, SchemaInspectionInput inspection)
    : fast_schema_(std::move(schema)), has_fast_(true) {
    inspection_ = inspect(inspection);

    if (inspection_.verdict == InspectionVerdict::kForceSlowPath) {
        path_ = EnginePath::kSlowFallback;
        return;
    }

#if IRIS_ENABLE_JIT
    jit_fn_ = jit::jit_compile(fast_schema_, jit_buffer_);
    if (jit_fn_) {
        path_ = EnginePath::kFastJit;
        return;
    }
#endif
    path_ = EnginePath::kFastInterpret;
}

Validator Validator::from_schema_json(std::string_view schema_json,
                                      ValidatorBuild& out_build) {
    Validator v;

    // 1) Fast Path
    auto fast = compile_schema_from_json(schema_json);
    if (fast.ok) {
        v.fast_schema_ = std::move(fast.schema);
        v.has_fast_ = true;
#if IRIS_ENABLE_JIT
        v.jit_fn_ = jit::jit_compile(v.fast_schema_, v.jit_buffer_);
        v.path_ = v.jit_fn_ ? EnginePath::kFastJit : EnginePath::kFastInterpret;
#else
        v.path_ = EnginePath::kFastInterpret;
#endif
        // restore fast path synthetic kinds (kAlwaysValid / kAlwaysInvalid)
        switch (v.fast_schema_.kind) {
            case SchemaKind::kAlwaysValid:   v.path_ = EnginePath::kAlwaysValid;   break;
            case SchemaKind::kAlwaysInvalid: v.path_ = EnginePath::kAlwaysInvalid; break;
            default: break;
        }
        out_build.ok = true;
        return v;
    }

    // 2) Slow Path
    auto slow = compile_slow_schema(schema_json);
    if (slow.ok) {
        v.slow_schema_ = std::move(slow.schema);
        v.path_ = EnginePath::kSlowFallback;
        out_build.ok = true;
        return v;
    }

    out_build.ok = false;
    out_build.diagnostic = "fast: " + fast.diagnostic + " | slow: " + slow.diagnostic;
    return v;
}

bool Validator::add_remote_document(std::string uri, std::string_view json) noexcept {
    if (!slow_schema_) {
        // If only fast path compiled, recompile slow schema
        // for remote injection. Fast path stays usable. Without stored schema_json,
        // cannot recompile — caller misuse; return false.
        return false;
    }
    return slow_schema_add_remote(*slow_schema_, std::move(uri), json);
}

ValidationReport Validator::validate(std::string_view json) const noexcept {
    switch (path_) {
        case EnginePath::kAlwaysValid: {
            ValidationReport r; return r;
        }
        case EnginePath::kAlwaysInvalid: {
            ValidationReport r; r.code = ValidationError::kTypeMismatch; return r;
        }
        case EnginePath::kSlowFallback:
            if (slow_schema_) return validate_slow_path(json, *slow_schema_);
            // legacy: Inspector veto with CompiledSchema only — explicit failure
            return validate_slow_path(json, fast_schema_);
        case EnginePath::kFastJit:
            if (jit_fn_) {
                return jit_fn_(reinterpret_cast<const std::uint8_t*>(json.data()),
                               json.size(), fast_schema_);
            }
            [[fallthrough]];
        case EnginePath::kFastInterpret:
        default:
            if (has_fast_) return ::iris::validate(json, fast_schema_);
            // neither fast nor slow → fail-safe
            ValidationReport r; r.code = ValidationError::kNotImplemented; return r;
    }
}

}  // namespace iris
