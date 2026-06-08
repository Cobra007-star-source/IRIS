// =============================================================================
// iris/slow_schema.hpp
//
// 慢车道的 schema 表达：完整保留 JSON Schema AST，外加预编译的索引
// （$ref / $defs 表 + 正则表达式缓存）。
//
// 设计原则
// ---------
//   1. **零信息丢失**：原始 JSON Schema 解析成 JsonValue 后整棵树都保留，
//      Fast Path 表达不了的关键字（$ref / allOf / pattern / ...）就靠这棵
//      AST 在 Slow Path 上跑递归解释器。
//   2. **预编译重对象**：$ref 解析表与 RE2 正则在 schema 编译阶段一次成型，
//      hot path 上只查指针、不再做字符串解析。
//   3. **零拷贝 hand-off**：与 Fast Path 共享同一段 JSON 输入 bytes。
//      Slow Path 自己用 JsonReader 再 parse 一次实例（必须，因为它需要
//      tree-shape 访问），但 schema 端没有任何二次拷贝。
//
// 公共 API（用法）
// ----------------
//     auto built = iris::compile_slow_schema(R"({"allOf":[...]})");
//     if (!built.ok) { ... 处理错误 ... }
//     auto r = iris::validate_slow_path(R"({"x":1})", *built.schema);
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "iris/json_reader.hpp"
#include "iris/parser.hpp"

namespace iris {

// 不透明编译后正则。具体表达由 src/slow_schema.cpp 决定（RE2 或 std::regex）。
// 头文件层面不暴露引擎类型，方便在不同 build configuration 间切换。
struct CompiledRegex;
struct CompiledRegexDeleter {
    void operator()(CompiledRegex* p) const noexcept;
};

// 编译产物。JsonValue 拥有所有子树；resources/regex_cache 仅保存指向子树的引用，
// SlowSchema 整体是 move-only。
//
// 跨文档 $ref（http(s):// 形式）通过 remote_roots + add_remote_document 支持：
//   1. 用户在编译完主 schema 后调用 add_remote_document(uri, json) 注入外部文档。
//   2. 每注入一份外部文档，就 owning 一棵 JsonValue 并把它的所有 $id / $anchor /
//      JSON-pointer 形态登记进 resources 表。
//   3. eval 期间 $ref 按 RFC 3986 解析为绝对 URI，去 resources 表查目标节点。
struct SlowSchema {
    SlowSchema();
    ~SlowSchema();
    SlowSchema(SlowSchema&&) noexcept;
    SlowSchema& operator=(SlowSchema&&) noexcept;

    // 原始 schema 文档（owned）
    JsonValue root;

    // 通过 add_remote_document() 注入的外部文档，owned。slow_eval 不直接访问，
    // 它们的内部节点已经通过 resources 表暴露。
    std::vector<std::unique_ptr<JsonValue>> remote_roots;

    // URI → subschema 节点 的解析表。key 取若干形态：
    //   "<absolute_uri>"              ：通过 $id 注册的整个 subschema
    //   "<absolute_uri>#<anchor>"     ：通过 $anchor / $dynamicAnchor 注册
    //   "<absolute_uri>#/path/.../"   ：通过 JSON Pointer 注册（每个子节点）
    //   ""                            ：主文档 root（兜底）
    //   "#/path/..."                  ：主文档 JSON Pointer（无 $id 时）
    //   "anchor:<name>"               ：旧版同文档 anchor 兼容键
    std::unordered_map<std::string, const JsonValue*> resources;

    // legacy 同文档 refs（保留兼容；与 resources 重叠）。
    std::unordered_map<std::string, const JsonValue*> refs;

    // 每个 subschema 对象节点 → 它的有效 base URI（来自最近的祖先 $id）。
    // 用于 $ref / $dynamicRef 在 hot path 上零拷贝读取 base，再做 RFC 3986 解析。
    std::unordered_map<const JsonValue*, std::string> node_base;

    // 主文档自身的 base URI（取 root.$id，否则为空字符串）。
    std::string primary_base;

    // 正则表达式缓存：每个含 "pattern" 的字符串子节点 → 已编译的正则。
    std::unordered_map<const JsonValue*,
                       std::unique_ptr<CompiledRegex, CompiledRegexDeleter>> regex_cache;

    // 最大递归深度（防御无限 $ref 展开）
    int max_eval_depth = 128;

    // 是否禁用 validation 词汇（来自 $schema 指向的 metaschema 的 $vocabulary
    // 声明）。-1=未检测；0=启用；1=禁用。lazy 在第一次 validate 时计算，
    // 因为 remote 文档可能在 compile 之后才注入。
    mutable int disable_validation_vocab = -1;
};

struct SlowSchemaBuildResult {
    bool                          ok = false;
    std::unique_ptr<SlowSchema>   schema;
    std::string                   diagnostic;
};

// 把一段 JSON Schema 文本编译为 SlowSchema：
//   1. 解析为 JsonValue
//   2. 递归扫描收集 $defs / 命名 $ref 节点
//   3. 编译所有 pattern 为 RE2
//   4. 检测出无法解决的引用（如 http:// 跨文档）→ ok=false
[[nodiscard]] SlowSchemaBuildResult compile_slow_schema(std::string_view schema_json);

// 主入口：慢车道校验。
//   - json_instance：原始 JSON 字节流，零拷贝；内部用 JsonReader 解析为 JsonValue 树
//   - schema：已编译的 SlowSchema
ValidationReport validate_slow_path(std::string_view json_instance,
                                    const SlowSchema& schema) noexcept;

// 注入一份外部文档，使 $ref 可以解析到它。uri 必须是绝对 URI（带 scheme）；
// json 是文档字面量。失败（JSON parse 错、URI 不合法）时静默返回 false，不抛。
// 同一个 uri 重复注入：后注入覆盖先注入。
bool slow_schema_add_remote(SlowSchema& s, std::string uri, std::string_view json) noexcept;

// 暴露：是否含有 RE2 编译期支持。fast/slow 两条路径无关，
// 仅用于 CI 与诊断输出。
[[nodiscard]] bool slow_path_has_re2() noexcept;

}  // namespace iris
