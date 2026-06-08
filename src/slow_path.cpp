// =============================================================================
// src/slow_path.cpp
//
// 慢车道：负责处理 Fast Path 表达不了的 JSON Schema 关键字
// （$ref / allOf / anyOf / oneOf / not / if-then-else / pattern / const / enum /
// multipleOf / patternProperties / dependent* / contains / prefixItems / ...）
//
// 实现位于 src/slow_path_eval.cpp：基于 JsonValue AST 的递归 keyword 解释器。
//
// 这个文件只保留旧的 "schema 仍按 CompiledSchema 的 SoA 表达" 的入口；该路径
// 现在仅在 Inspector 触发"强制慢车道"时被调用。SoA 已经丢掉 $ref / allOf 等
// 信息，所以这里**显式失败而不是悄悄回退到 Fast Path**——这是 v0.2 之前最危险
// 的 bug：Inspector 投否决票后慢车道默默返回 Fast Path 的近似答案，让人误以
// 为"已支持"。现在改为显式 kNotImplemented，等同于"slow path 拒绝服务"。
//
// 真正的"零拷贝双引擎"在 slow_path_eval.cpp + validator.cpp 里：
//   Validator 同时持有 CompiledSchema（fast 路径用）和 JsonValue（slow 路径用），
//   对一份 JSON 实例的 (data, size) 两条引擎都直接读，不做二次拷贝。
// =============================================================================
#include "iris/slow_path.hpp"

namespace iris {

ValidationReport validate_slow_path(std::string_view /*json*/,
                                    const CompiledSchema& /*schema*/) noexcept {
    ValidationReport r;
    r.code = ValidationError::kNotImplemented;
    return r;
}

}  // namespace iris
