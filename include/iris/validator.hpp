// =============================================================================
// iris/validator.hpp
//
// 公共 API：双引擎入口。
//
// 用法：
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
    // 经典入口：直接接收已经走过 Fast Path 的 CompiledSchema。保留兼容。
    explicit Validator(CompiledSchema schema,
                       SchemaInspectionInput inspection = {});

    // 新入口（推荐）：直接吃 schema JSON 字符串，自动做 fast↔slow 路由。
    //   1. 先尝试 compile_schema_from_json（Fast Path，CompiledSchema/SoA）
    //   2. 若 Fast Path 拒绝（含 unsupported keyword），fallback 到
    //      compile_slow_schema（Slow Path，JsonValue AST + RE2 缓存 + $ref 表）
    //   3. 若两者都失败，build.ok=false，diagnostic 给原因
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

    // 把一份外部文档注册进当前 Validator 的 Slow 引擎，使后续 $ref 可解析。
    // 仅在 has_slow_path() 为 true 时生效；如果当前路径只走 Fast，会自动
    // 把 schema 切回 Slow（重新编译 schema 失败时这一项会安静失败）。
    // 返回是否成功登记。
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
