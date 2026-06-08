// =============================================================================
// iris/parser.hpp
//
// Fused Parse & Validate 引擎 (Phase 2 Fast Path)
//
// 设计原则（来自白皮书）：
//
//   - Zero-DOM：不在堆上建立 JSON 树，全部在原始 buffer 上扫描
//   - Zero Allocation：所有状态都在栈上 / 调用方提供的 arena 上
//   - Bitwise DFA：用类型位掩码替代 if/else 链
//   - Perfect Hash：字段名 → slot 的 O(1) 跳转
//
// 当前阶段限制（明确写在文档里，方便后续 Phase 抬升）：
//
//   - 仅校验单层 object schema
//   - 数值解析为 int64 / double 二选一，不实现 IEEE-754 精度恢复
//   - 字符串不做 \u 转义解码（按字节统计长度）
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
    // 慢车道相关
    kNotImplemented,     // 该 schema 关键字尚未支持（落到慢车道但慢车道还未实现）
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
    kSlowSchemaInvalid,  // schema JSON 本身有问题
};

const char* validation_error_name(ValidationError e) noexcept;

struct ValidationReport {
    ValidationError code = ValidationError::kOk;
    // 出错位置：在原始 buffer 上的字节偏移
    std::uint32_t   offset = 0;
    // 出错字段（如已识别）：在 schema field 列表中的 slot；-1 表示未关联具体字段
    std::int32_t    field_slot = -1;
    // 见过的字段位掩码（调试用）
    std::uint64_t   seen_mask = 0;

    [[nodiscard]] bool ok() const noexcept { return code == ValidationError::kOk; }
};

// 主入口：fused 校验 - 不返回任何 DOM、不分配
//
// data: 原始 JSON 字节流（无需 NUL 终止）
// size: 字节长度
// schema: 已编译的 CompiledSchema
ValidationReport validate(const std::uint8_t* IRIS_RESTRICT data,
                          std::size_t size,
                          const CompiledSchema& schema) noexcept;

[[nodiscard]] IRIS_FORCE_INLINE ValidationReport
validate(std::string_view view, const CompiledSchema& schema) noexcept {
    return validate(reinterpret_cast<const std::uint8_t*>(view.data()),
                    view.size(), schema);
}

}  // namespace iris
