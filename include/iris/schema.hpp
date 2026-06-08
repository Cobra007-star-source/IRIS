// =============================================================================
// iris/schema.hpp
//
// 编译期 / 加载期得到的 Schema 描述符。
//
// 一切都是 SoA、POD、可拷贝复制：
//
//   types[i]          : 该字段允许的类型掩码 (bitwise DFA 的 Expected Mask)
//   required_mask     : 必填位掩码（bit i 表示 slot i 必填）
//   max_string_len[i] : 字符串字段的长度上限（0 = 无）
//   min_int[i]/max_int[i] : 整数字段的边界
//
// Schema 越简单，bitwise DFA 越紧。复杂 schema 由 Inspector 在加载期拦截，
// 直接进入慢车道。
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

struct CompiledSchema;  // 前向：因为 CompiledSchema 自递归引用

// 类型掩码：低 8 位与 TokenKind 对齐
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

// "短键直查表"
//
// 当 schema 所有字段名长度 ≤ 8 字节时（绝大多数实际 schema 满足），
// 完美哈希被替换为更直接的方案：
//   - 把每个字段名打包为 1 个 uint64_t (little-endian, 高位补 0)
//   - 查询时一次 8 字节 unaligned load + 长度掩码 + 线性比较
//
// 这条快路径每个 key 节省 ~30 cycles (fmix64 + mod)。
//
// 调用前必须保证 (key_ptr + 8) 仍在合法可读范围内——
// iris_validate 中通过追加 16 字节零填充满足。
struct ShortKeyTable {
    static constexpr std::size_t kMaxKeys = 16;
    std::uint64_t bits[kMaxKeys] = {};
    std::uint8_t  lens[kMaxKeys] = {};
    std::int8_t   slot[kMaxKeys] = {};
    std::uint8_t  count = 0;
};

// CompiledSchema 的"根类型"：决定 validate() 入口怎么 dispatch
enum class SchemaKind : std::uint8_t {
    kAlwaysValid   = 0,   // schema == true 或 {}
    kAlwaysInvalid = 1,   // schema == false
    kObjectRoot    = 2,   // 顶层 type=object，递归走 validate_object
    kValueRoot     = 3,   // 顶层 type 为 string/number/integer/array/bool/null
};

// 编译产物：递归 object/array schema。
//
// SchemaKind 决定 root 行为；
// 对 kObjectRoot：types[i] 是第 i 个 property 的允许类型 mask；
// 对 kValueRoot ：只用 slot 0 存 root 的 type+constraints（视为合成"单值"slot）；
// 对 kAlways*   ：全字段为空，validate 直接返回。
//
// 嵌套字段：
//   - 字段 type 包含 object → nested_object[slot] 持有子 schema
//   - 字段 type 包含 array  → array_item_type[slot] 是 item 的 TypeMask；
//                              若 item 还是 object，则 array_item_nested[slot] 进一步递归
//
// 三个并行 vector 维持 SoA 风格；非嵌套字段的对应槽位用空指针 / kTypeNone 占位。
struct CompiledSchema {
    SchemaKind                 kind = SchemaKind::kObjectRoot;
    PerfectHashTable           field_index;
    ShortKeyTable              short_keys;
    std::vector<TypeMask>      types;
    std::vector<std::uint32_t> min_string_len;
    std::vector<std::uint32_t> max_string_len;
    std::vector<std::int64_t>  min_int;
    std::vector<std::int64_t>  max_int;
    // 浮点最小/最大：与 min_int/max_int 并行，仅在 number 字段需要时启用。
    // 默认 -DBL_MAX / +DBL_MAX 表达"无约束"；不用 infinity() 因为 -ffast-math 会把
    // numeric_limits::infinity() 折成 0（fast-math 假定 finite-math-only）。
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

// 受限 Schema 字段描述符，供 DSL 使用（不含嵌套；嵌套请走 compile_schema_from_json）
struct FieldSpec {
    std::string_view name;
    TypeMask         type           = kTypeNone;
    bool             required       = false;
    std::uint32_t    min_string_len = 0;
    std::uint32_t    max_string_len = 0;
    std::int64_t     min_int        = INT64_MIN;
    std::int64_t     max_int        = INT64_MAX;
};

// 用 FieldSpec 数组直接构造 CompiledSchema 的便利函数。
// 失败时 ok=false，diagnostic 给出原因（例如完美哈希构造不出来）。
struct SchemaBuildResult {
    bool             ok = false;
    CompiledSchema   schema;
    std::string      diagnostic;
};

SchemaBuildResult compile_schema(std::span<const FieldSpec> fields,
                                 bool additional_properties = false);

// 直接从 JSON Schema 文档（即一段 schema.json 的文本）编译。
//
// 目前覆盖 draft-2020-12 的核心子集：
//   - type: 单类型 / 类型数组
//   - properties / required / additionalProperties
//   - minimum / maximum (integer)
//   - minLength / maxLength
//
// 不支持的关键字（pattern / $ref / allOf 等）会被 Inspector 投否决票
// 后续由慢车道接手；当前 stub 实现是直接拒绝。
SchemaBuildResult compile_schema_from_json(std::string_view schema_json);

}  // namespace iris
