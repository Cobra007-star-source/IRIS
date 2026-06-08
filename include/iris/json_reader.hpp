// =============================================================================
// iris/json_reader.hpp
//
// 通用 JSON 读取器 (bootstrap 用途)
//
// 设计定位：
//   - 不在热路径上。仅在 schema 编译期解析 schema.json
//   - 因此牺牲性能换 API 友好度：返回 std::variant 风格的 JsonValue
//   - 没有 SAX、没有 DOD，10MB schema 解析 <1ms 已经远超需求
//
// 如果将来要"用 IRIS 解析 IRIS schema"，把这块换成 fused pipeline 即可。
// =============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace iris {

class JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray  = std::vector<JsonValue>;

class JsonValue {
public:
    enum class Type : std::uint8_t {
        kNull, kBool, kInt, kDouble, kString, kArray, kObject
    };

    JsonValue() = default;
    static JsonValue make_null()                       { return JsonValue(Type::kNull); }
    static JsonValue make_bool(bool b)                 { JsonValue v(Type::kBool);   v.b_ = b;          return v; }
    static JsonValue make_int(std::int64_t i)          { JsonValue v(Type::kInt);    v.i_ = i;          return v; }
    static JsonValue make_double(double d)             { JsonValue v(Type::kDouble); v.d_ = d;          return v; }
    static JsonValue make_string(std::string s)        { JsonValue v(Type::kString); v.s_ = std::move(s); return v; }
    static JsonValue make_array(JsonArray a)           { JsonValue v(Type::kArray);  v.a_ = std::make_unique<JsonArray>(std::move(a)); return v; }
    static JsonValue make_object(JsonObject o)         { JsonValue v(Type::kObject); v.o_ = std::make_unique<JsonObject>(std::move(o)); return v; }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null()   const noexcept { return type_ == Type::kNull; }
    [[nodiscard]] bool is_bool()   const noexcept { return type_ == Type::kBool; }
    [[nodiscard]] bool is_int()    const noexcept { return type_ == Type::kInt; }
    [[nodiscard]] bool is_double() const noexcept { return type_ == Type::kDouble; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::kInt || type_ == Type::kDouble; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::kString; }
    [[nodiscard]] bool is_array()  const noexcept { return type_ == Type::kArray; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::kObject; }

    [[nodiscard]] bool                as_bool()   const noexcept { return b_; }
    [[nodiscard]] std::int64_t        as_int()    const noexcept { return type_ == Type::kInt ? i_ : static_cast<std::int64_t>(d_); }
    [[nodiscard]] double              as_double() const noexcept { return type_ == Type::kDouble ? d_ : static_cast<double>(i_); }
    [[nodiscard]] const std::string&  as_string() const noexcept { return s_; }
    [[nodiscard]] const JsonArray&    as_array()  const noexcept { return *a_; }
    [[nodiscard]] const JsonObject&   as_object() const noexcept { return *o_; }

    // 在 object 中找 key，未找到返回 nullptr。线性扫描（schema 字段少，O(N) OK）。
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept {
        if (!o_) return nullptr;
        for (auto& [k, v] : *o_) if (k == key) return &v;
        return nullptr;
    }

private:
    explicit JsonValue(Type t) : type_(t) {}

    Type                          type_ = Type::kNull;
    bool                          b_    = false;
    std::int64_t                  i_    = 0;
    double                        d_    = 0.0;
    std::string                   s_;
    std::unique_ptr<JsonArray>    a_;
    std::unique_ptr<JsonObject>   o_;
};

struct JsonParseResult {
    bool         ok = false;
    JsonValue    value;
    std::string  diagnostic;  // 仅在 ok==false 时填充
    std::size_t  error_offset = 0;
};

// 解析整段 JSON 文档。允许尾部 whitespace。
[[nodiscard]] JsonParseResult parse_json(std::string_view text);

}  // namespace iris
