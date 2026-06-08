// =============================================================================
// iris/slow_path.hpp  (Phase 4 骨架)
//
// 慢车道：被 Inspector 拦截的 schema 走通用解释器，保证 Bowtie 100% 通过。
//
// 当前实现：占位的解释器接口，行为是“原样接受”，仅作 API 形状定型。
//
// TODO(phase-4): 接入 QuickJS 或自研 AST 遍历器。
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>

#include "iris/parser.hpp"
#include "iris/schema.hpp"

namespace iris {

ValidationReport validate_slow_path(std::string_view json,
                                    const CompiledSchema& schema) noexcept;

}  // namespace iris
