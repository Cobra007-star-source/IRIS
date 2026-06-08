// =============================================================================
// src/validator.cpp
//
// 双引擎路由：
//   * Validator(CompiledSchema)              — 直接走 Fast Path（含可选 JIT）
//   * Validator::from_schema_json(text, out) — 自动选路：
//       (a) 先 compile_schema_from_json → Fast Path
//       (b) 失败再 compile_slow_schema   → Slow Path（AST 解释器）
//       (c) 还失败 → ValidatorBuild.ok = false
//
// 数据共享：两条路径都对原始 json 字节流做 zero-copy（fast 直接 SIMD 扫，slow
// 经 JsonReader 解析为 JsonValue 树。后者必须 parse 因为它需要 tree-shape 访问，
// 但 schema 编译产物完全独立、互不浪费）。
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
        // 还原 Fast Path 的合成 kind（kAlwaysValid / kAlwaysInvalid）
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
        // 如果当前 Validator 只编译了 Fast Path，需要把 schema 再编一遍 Slow，
        // 以便注入远端。Fast 仍然保留可用。如果 fast 自带的 schema_json 没存，
        // 我们没法 re-compile —— 此时认为调用方使用错误，安静返回 false。
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
            // legacy 通道：Inspector 投否决但只持有 CompiledSchema —— 显式失败
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
            // 既没 fast 也没 slow → fail-safe
            ValidationReport r; r.code = ValidationError::kNotImplemented; return r;
    }
}

}  // namespace iris
