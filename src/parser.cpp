// =============================================================================
// src/parser.cpp
//
// Fused Parse & Validate kernel — 递归版（Phase 4 升级）
//
// 入口：validate() → validate_object()
//
// 三个互相递归的内部函数：
//
//   validate_object(c, schema)
//        当 c.peek()=='{' 时进入。展开字段 → key 解析 → value dispatch。
//        遇到 properties[k].type=object 且 nested_object[k]!=null → 递归 validate_object()
//        遇到 properties[k].type=array  且 array_item_type[k]!=0   → 递归 validate_array()
//
//   validate_array(c, item_type, item_nested)
//        当 c.peek()=='[' 时进入。逐元素 dispatch → 类型校验。
//        item 若为 object 且 item_nested!=null → 递归 validate_object()
//        item 若为 array 当前 Fast Path 仅做结构跳过（数组嵌套数组属长尾，留 Phase 5）。
//
//   validate_value(c, constraint)
//        single value dispatcher，统一处理标量 + 嵌套递归。
//
// 错误一旦触发立即返回，沿调用栈逐级冒泡——天然的"错误熔断"。
// =============================================================================
#include "iris/parser.hpp"

#include <cfloat>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

// Count Unicode codepoints in a JSON-encoded string body (without surrounding quotes).
// 输入是 scan_string 返回的原始字节范围——含 \" / \\ / \u.... 等 escape sequence。
// Surrogate pair (\uD800-DBFF 后跟 \uDC00-DFFF) 视为 1 codepoint。
// 与 spec "string length is the number of characters" 对齐。
inline std::uint32_t json_string_codepoints(const std::uint8_t* p, std::uint32_t n) noexcept {
    auto hex_val = [](std::uint8_t c) noexcept -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto parse_hex4 = [&](std::uint32_t pos) noexcept -> std::uint32_t {
        return (std::uint32_t(hex_val(p[pos]))   << 12) |
               (std::uint32_t(hex_val(p[pos+1])) << 8)  |
               (std::uint32_t(hex_val(p[pos+2])) << 4)  |
                std::uint32_t(hex_val(p[pos+3]));
    };
    std::uint32_t count = 0;
    std::uint32_t i = 0;
    while (i < n) {
        std::uint8_t c = p[i];
        if (c == '\\' && i + 1 < n) {
            if (p[i+1] == 'u' && i + 5 < n) {
                std::uint32_t hi = parse_hex4(i + 2);
                if (hi >= 0xD800 && hi <= 0xDBFF &&
                    i + 11 < n && p[i+6] == '\\' && p[i+7] == 'u') {
                    std::uint32_t lo = parse_hex4(i + 8);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        ++count; i += 12; continue;
                    }
                }
                ++count; i += 6; continue;
            }
            ++count; i += 2; continue;   // \", \\, \n, ...
        }
        if ((c & 0xC0) != 0x80) ++count;
        ++i;
    }
    return count;
}

}  // namespace

#include "iris/simd_ops.hpp"

namespace iris {

namespace {

IRIS_FORCE_INLINE TypeMask token_to_type(std::uint8_t c) noexcept {
    switch (c) {
        case '{': return kTypeObject;
        case '[': return kTypeArray;
        case '"': return kTypeString;
        case 't':
        case 'f': return kTypeBoolean;
        case 'n': return kTypeNull;
        default:
            if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
                return kTypeNumber | kTypeInteger;
            }
            return kTypeNone;
    }
}

IRIS_FORCE_INLINE bool is_digit(std::uint8_t c) noexcept {
    return c >= '0' && c <= '9';
}

struct Cursor {
    const std::uint8_t* data;
    std::size_t         size;
    std::size_t         pos;

    IRIS_FORCE_INLINE bool        eof()  const noexcept { return pos >= size; }
    IRIS_FORCE_INLINE std::uint8_t peek() const noexcept { return data[pos]; }

    IRIS_FORCE_INLINE void skip_ws() noexcept {
        pos += simd::skip_json_whitespace(data + pos, size - pos);
    }
};

// 单 SIMD pass 字符串扫描（仅闭合 '"' 与转义 '\\' 在一次扫描中定位）
[[nodiscard]] IRIS_FORCE_INLINE bool scan_string(Cursor& c, std::uint32_t& out_off,
                                                 std::uint32_t& out_len) noexcept {
    ++c.pos;
    out_off = static_cast<std::uint32_t>(c.pos);
    while (c.pos < c.size) {
        std::size_t rest = c.size - c.pos;
        std::size_t hit  = simd::find_byte_pair(c.data + c.pos, rest, '"', '\\');
        if (IRIS_UNLIKELY(hit == rest)) return false;
        std::uint8_t ch = c.data[c.pos + hit];
        c.pos += hit;
        if (IRIS_LIKELY(ch == '"')) {
            out_len = static_cast<std::uint32_t>(c.pos - out_off);
            ++c.pos;
            return true;
        }
        c.pos += 2;  // '\\' + escapee
    }
    return false;
}

// 通用括号平衡跳过：当 nested 信息不可用时退回到结构性校验
[[nodiscard]] bool skip_balanced(Cursor& c) noexcept {
    if (c.eof()) return false;
    std::uint8_t open = c.peek();
    std::uint8_t close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (c.pos < c.size) {
        std::uint8_t ch = c.data[c.pos];
        if (ch == '"') {
            std::uint32_t off, len;
            if (!scan_string(c, off, len)) return false;
            continue;
        }
        if (ch == '{' || ch == '[') ++depth;
        else if (ch == '}' || ch == ']') {
            --depth;
            ++c.pos;
            if (depth == 0) return ch == close;
            continue;
        }
        ++c.pos;
    }
    return false;
}

struct NumberScan {
    bool         is_integer  = false;   // lexical 形态：无小数点 / 无指数
    bool         is_number   = false;
    std::int64_t int_value   = 0;
    double       dbl_value   = 0.0;
    std::size_t  end_pos     = 0;
};

NumberScan scan_number(const std::uint8_t* data, std::size_t size, std::size_t pos) noexcept {
    NumberScan r;
    std::size_t start = pos;
    bool neg = false;
    if (pos < size && data[pos] == '-') { neg = true; ++pos; }
    else if (pos < size && data[pos] == '+') { ++pos; }
    std::size_t int_start = pos;
    while (pos < size && is_digit(data[pos])) ++pos;
    if (pos == int_start) { r.end_pos = pos; return r; }
    bool has_dot_or_exp = false;
    if (pos < size && data[pos] == '.') {
        has_dot_or_exp = true;
        ++pos;
        while (pos < size && is_digit(data[pos])) ++pos;
    }
    if (pos < size && (data[pos] == 'e' || data[pos] == 'E')) {
        has_dot_or_exp = true;
        ++pos;
        if (pos < size && (data[pos] == '+' || data[pos] == '-')) ++pos;
        while (pos < size && is_digit(data[pos])) ++pos;
    }
    r.is_number  = true;
    r.is_integer = !has_dot_or_exp;
    r.end_pos    = pos;
    if (r.is_integer) {
        std::uint64_t acc = 0;
        bool overflow = false;
        for (std::size_t i = int_start; i < pos; ++i) {
            std::uint64_t next = acc * 10 + (data[i] - '0');
            if (next < acc) { overflow = true; break; }
            acc = next;
        }
        if (overflow) {
            r.int_value = neg ? std::numeric_limits<std::int64_t>::min()
                              : std::numeric_limits<std::int64_t>::max();
        } else if (neg) {
            r.int_value = -static_cast<std::int64_t>(acc);
        } else {
            r.int_value = static_cast<std::int64_t>(acc);
        }
        r.dbl_value = static_cast<double>(r.int_value);
    } else {
        // 浮点：用 from_chars 解析；失败的话退化为 0（不会影响 type 校验的正确性）
        auto first = reinterpret_cast<const char*>(data + start);
        auto last  = reinterpret_cast<const char*>(data + pos);
        std::from_chars(first, last, r.dbl_value);
    }
    return r;
}

IRIS_FORCE_INLINE bool match_true(const std::uint8_t* p, std::size_t rem) noexcept {
    if (IRIS_UNLIKELY(rem < 4)) return false;
    std::uint32_t v; std::memcpy(&v, p, 4);
    return v == 0x65757274u;
}
IRIS_FORCE_INLINE bool match_null(const std::uint8_t* p, std::size_t rem) noexcept {
    if (IRIS_UNLIKELY(rem < 4)) return false;
    std::uint32_t v; std::memcpy(&v, p, 4);
    return v == 0x6c6c756eu;
}
IRIS_FORCE_INLINE bool match_false(const std::uint8_t* p, std::size_t rem) noexcept {
    if (IRIS_UNLIKELY(rem < 5)) return false;
    std::uint32_t v; std::memcpy(&v, p, 4);
    return v == 0x736c6166u && p[4] == 'e';
}

ValidationReport make_err(ValidationError code, std::size_t pos,
                          std::int32_t slot = -1,
                          std::uint64_t seen = 0) noexcept {
    return {code, static_cast<std::uint32_t>(pos), slot, seen};
}

ValidationReport make_ok(std::size_t pos, std::uint64_t seen = 0) noexcept {
    return {ValidationError::kOk, static_cast<std::uint32_t>(pos), -1, seen};
}

ValidationReport validate_object(Cursor& c, const CompiledSchema& schema) noexcept;
ValidationReport validate_array(Cursor& c, TypeMask item_type,
                                const CompiledSchema* item_nested) noexcept;

// -----------------------------------------------------------------------------
// dispatch_value
//
// 故意写成宏 + 大 switch 块：避免函数边界扼杀内联。
// 当前 value 的所有约束以局部变量出现在调用点，避免 ValueConstraint 整体构造。
//
// 退出条件：
//   - 失败：在主调函数里 return 错误
//   - 成功：cursor 已推过该 value，继续后续 ',' / '}' 判定
// -----------------------------------------------------------------------------
//
// 用 #define 是经过权衡的：
//   - inline 函数：编译器对递归函数禁用内联，标量路径性能掉 30%
//   - lambda：捕获太多变量，反而劣化
//   - 宏：直接展开到调用点，无成本
//
// 调用上下文必须暴露：
//   c, schema(unused for array), allowed, obj_sub, item_type, item_nested,
//   min_string_len, max_string_len, min_int, max_int, slot, seen_mask
// 失败处理通过 `return make_err(...)` 体现。
//
// 宏会展开为：根据 c.peek() 的值跳转处理；失败 return，成功 break 后继续。
//
// 字符串长度按 UTF-8 codepoint 计数（spec 要求）。我们只在 schema 显式设了
// min/maxLength 时才扫一遍计 codepoint —— ASCII-only schema 上 zero cost。
#define IRIS_VALIDATE_SCALAR_AT(head_)                                              \
    do {                                                                             \
        switch (head_) {                                                             \
            case '"': {                                                              \
                if (!(allowed & kTypeString))                                        \
                    return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                std::uint32_t s_off, s_len;                                          \
                if (!scan_string(c, s_off, s_len))                                   \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                if (min_string_len > 0 || max_string_len > 0) {                      \
                    std::uint32_t cp = ::json_string_codepoints(c.data + s_off, s_len); \
                    if (cp < min_string_len)                                         \
                        return make_err(ValidationError::kStringTooShort, s_off, slot, seen_mask); \
                    if (max_string_len > 0 && cp > max_string_len)                   \
                        return make_err(ValidationError::kStringTooLong, s_off, slot, seen_mask); \
                }                                                                    \
                break;                                                                \
            }                                                                         \
            case 't': {                                                               \
                if (!(allowed & kTypeBoolean))                                        \
                    return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                if (!match_true(c.data + c.pos, c.size - c.pos))                      \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                c.pos += 4; break;                                                    \
            }                                                                         \
            case 'f': {                                                               \
                if (!(allowed & kTypeBoolean))                                        \
                    return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                if (!match_false(c.data + c.pos, c.size - c.pos))                     \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                c.pos += 5; break;                                                    \
            }                                                                         \
            case 'n': {                                                               \
                if (!(allowed & kTypeNull))                                           \
                    return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                if (!match_null(c.data + c.pos, c.size - c.pos))                      \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                c.pos += 4; break;                                                    \
            }                                                                         \
            case '{': {                                                               \
                if (!(allowed & kTypeObject))                                         \
                    return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                if (obj_sub) {                                                        \
                    auto _r = validate_object(c, *obj_sub);                           \
                    if (IRIS_UNLIKELY(!_r.ok())) return _r;                           \
                } else if (!skip_balanced(c)) {                                       \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                }                                                                     \
                break;                                                                \
            }                                                                         \
            case '[': {                                                               \
                if (!(allowed & kTypeArray))                                          \
                    return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                if (item_type != 0) {                                                 \
                    auto _r = validate_array(c, item_type, item_nested);              \
                    if (IRIS_UNLIKELY(!_r.ok())) return _r;                           \
                } else if (!skip_balanced(c)) {                                       \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                }                                                                     \
                break;                                                                \
            }                                                                         \
            default: {                                                                \
                TypeMask vt = token_to_type(head_);                                   \
                if (vt == kTypeNone)                                                  \
                    return make_err(ValidationError::kUnexpectedToken, c.pos, slot, seen_mask); \
                NumberScan ns = scan_number(c.data, c.size, c.pos);                   \
                if (!ns.is_number)                                                    \
                    return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask); \
                if (ns.is_integer) {                                                  \
                    if (!(allowed & (kTypeInteger | kTypeNumber)))                    \
                        return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                } else {                                                              \
                    if (!(allowed & kTypeNumber))                                     \
                        return make_err(ValidationError::kTypeMismatch, c.pos, slot, seen_mask); \
                }                                                                     \
                if (ns.dbl_value < min_dbl || ns.dbl_value > max_dbl)                 \
                    return make_err(ValidationError::kIntOutOfRange, c.pos, slot, seen_mask); \
                c.pos = ns.end_pos;                                                   \
                break;                                                                \
            }                                                                         \
        }                                                                             \
    } while (0)

// -----------------------------------------------------------------------------
// validate_array
//
// 处理任意 item_type；item 若为 object 且 item_nested!=null → 递归 validate_object()。
// 内部 loop 局部变量为 item 路径的"伪 slot=-1"约束。
// -----------------------------------------------------------------------------
ValidationReport validate_array(Cursor& c, TypeMask item_type,
                                const CompiledSchema* item_nested) noexcept {
    if (IRIS_UNLIKELY(c.eof() || c.peek() != '[')) {
        return make_err(ValidationError::kUnexpectedToken, c.pos);
    }
    ++c.pos;
    c.skip_ws();
    if (!c.eof() && c.peek() == ']') { ++c.pos; return make_ok(c.pos); }

    TypeMask              allowed           = item_type;
    const CompiledSchema* obj_sub           = (item_type & kTypeObject) ? item_nested : nullptr;
    TypeMask              inner_item_type   = 0;     // array-of-array Phase 5
    const CompiledSchema* inner_item_nested = nullptr;
    std::uint32_t         min_string_len    = 0;
    std::uint32_t         max_string_len    = 0;
    double                min_dbl           = -DBL_MAX;
    double                max_dbl           =  DBL_MAX;
    std::int32_t          slot              = -1;
    std::uint64_t         seen_mask         = 0;
    (void)inner_item_nested; (void)inner_item_type;
    TypeMask              item_type_local   = inner_item_type;
    const CompiledSchema* item_nested_local = inner_item_nested;
    (void)item_type_local; (void)item_nested_local;

    while (true) {
        c.skip_ws();
        if (c.eof()) return make_err(ValidationError::kInvalidJson, c.pos);
        std::uint8_t head = c.peek();
        // 宏依赖名为 item_type / item_nested 的本地变量；用同名变量映射
        TypeMask              item_type      = inner_item_type;
        const CompiledSchema* item_nested    = inner_item_nested;
        IRIS_VALIDATE_SCALAR_AT(head);
        c.skip_ws();
        if (c.eof()) return make_err(ValidationError::kInvalidJson, c.pos);
        if (c.peek() == ',') { ++c.pos; continue; }
        if (c.peek() == ']') { ++c.pos; return make_ok(c.pos); }
        return make_err(ValidationError::kUnexpectedToken, c.pos);
    }
}

// -----------------------------------------------------------------------------
// validate_object
// -----------------------------------------------------------------------------
ValidationReport validate_object(Cursor& c, const CompiledSchema& schema) noexcept {
    if (IRIS_UNLIKELY(c.eof() || c.peek() != '{')) {
        return make_err(ValidationError::kUnexpectedToken, c.pos);
    }
    ++c.pos;
    c.skip_ws();

    std::uint64_t seen_mask = 0;
    if (!c.eof() && c.peek() == '}') {
        ++c.pos;
        if ((seen_mask & schema.required_mask) != schema.required_mask)
            return make_err(ValidationError::kMissingRequired, c.pos, -1, seen_mask);
        return make_ok(c.pos, seen_mask);
    }

    while (true) {
        c.skip_ws();
        if (c.eof() || c.peek() != '"')
            return make_err(ValidationError::kUnexpectedToken, c.pos, -1, seen_mask);

        std::uint32_t key_off, key_len;
        if (!scan_string(c, key_off, key_len))
            return make_err(ValidationError::kInvalidJson, c.pos, -1, seen_mask);

        std::int32_t slot = -1;
        const auto& sk = schema.short_keys;
        if (IRIS_LIKELY(sk.count != 0 && key_len <= 8 &&
                        key_off + 8 <= static_cast<std::uint32_t>(c.size))) {
            std::uint64_t w;
            std::memcpy(&w, c.data + key_off, 8);
            std::uint64_t mask = (key_len == 8) ? ~0ULL : ((1ULL << (key_len * 8)) - 1);
            w &= mask;
            for (std::uint8_t i = 0; i < sk.count; ++i) {
                if (sk.lens[i] == key_len && sk.bits[i] == w) { slot = sk.slot[i]; break; }
            }
        } else {
            std::string_view key(reinterpret_cast<const char*>(c.data + key_off), key_len);
            slot = schema.field_index.lookup(key);
        }

        if (slot < 0) {
            if (!schema.additional_properties)
                return make_err(ValidationError::kUnknownField, key_off, -1, seen_mask);
        } else {
            std::uint64_t bit = 1ULL << slot;
            if (seen_mask & bit)
                return make_err(ValidationError::kDuplicateField, key_off, slot, seen_mask);
            seen_mask |= bit;
        }

        c.skip_ws();
        if (c.eof() || c.peek() != ':')
            return make_err(ValidationError::kUnexpectedToken, c.pos, slot, seen_mask);
        ++c.pos;
        c.skip_ws();
        if (c.eof()) return make_err(ValidationError::kInvalidJson, c.pos, slot, seen_mask);

        // 把 slot 的约束取出到一组寄存器变量（无堆查询，全在 L1）
        TypeMask              allowed        = (slot >= 0) ? schema.types[slot] : static_cast<TypeMask>(0xFF);
        const CompiledSchema* obj_sub        = (slot >= 0) ? schema.nested_object[slot].get() : nullptr;
        TypeMask              item_type      = (slot >= 0) ? schema.array_item_type[slot] : 0;
        const CompiledSchema* item_nested    = (slot >= 0) ? schema.array_item_nested[slot].get() : nullptr;
        std::uint32_t         min_string_len = (slot >= 0) ? schema.min_string_len[slot] : 0;
        std::uint32_t         max_string_len = (slot >= 0) ? schema.max_string_len[slot] : 0;
        double                min_dbl        = (slot >= 0) ? schema.min_dbl[slot] : -DBL_MAX;
        double                max_dbl        = (slot >= 0) ? schema.max_dbl[slot] :  DBL_MAX;

        std::uint8_t head = c.peek();
        IRIS_VALIDATE_SCALAR_AT(head);

        c.skip_ws();
        if (c.eof()) return make_err(ValidationError::kInvalidJson, c.pos, -1, seen_mask);
        if (c.peek() == ',') { ++c.pos; continue; }
        if (c.peek() == '}') {
            ++c.pos;
            if ((seen_mask & schema.required_mask) != schema.required_mask)
                return make_err(ValidationError::kMissingRequired, c.pos, -1, seen_mask);
            return make_ok(c.pos, seen_mask);
        }
        return make_err(ValidationError::kUnexpectedToken, c.pos, -1, seen_mask);
    }
}

#undef IRIS_VALIDATE_SCALAR_AT

}  // namespace

const char* validation_error_name(ValidationError e) noexcept {
    switch (e) {
        case ValidationError::kOk:                   return "ok";
        case ValidationError::kInvalidJson:          return "invalid_json";
        case ValidationError::kUnexpectedToken:      return "unexpected_token";
        case ValidationError::kUnknownField:         return "unknown_field";
        case ValidationError::kMissingRequired:      return "missing_required";
        case ValidationError::kTypeMismatch:         return "type_mismatch";
        case ValidationError::kStringTooShort:       return "string_too_short";
        case ValidationError::kStringTooLong:        return "string_too_long";
        case ValidationError::kIntOutOfRange:        return "int_out_of_range";
        case ValidationError::kDuplicateField:       return "duplicate_field";
        case ValidationError::kNotImplemented:       return "not_implemented";
        case ValidationError::kConstMismatch:        return "const_mismatch";
        case ValidationError::kEnumMismatch:         return "enum_mismatch";
        case ValidationError::kMultipleOf:           return "multiple_of";
        case ValidationError::kPatternMismatch:      return "pattern_mismatch";
        case ValidationError::kArrayTooShort:        return "array_too_short";
        case ValidationError::kArrayTooLong:         return "array_too_long";
        case ValidationError::kArrayNotUnique:       return "array_not_unique";
        case ValidationError::kArrayContainsViolation: return "array_contains";
        case ValidationError::kAllOfFailed:          return "allOf_failed";
        case ValidationError::kAnyOfFailed:          return "anyOf_failed";
        case ValidationError::kOneOfFailed:          return "oneOf_failed";
        case ValidationError::kNotFailed:            return "not_failed";
        case ValidationError::kIfThenElseFailed:     return "if_then_else_failed";
        case ValidationError::kDependentRequired:    return "dependent_required";
        case ValidationError::kSlowSchemaInvalid:    return "slow_schema_invalid";
    }
    return "unknown";
}

namespace {

// 跑一个 value root schema：cursor 处当前是任意 JSON value 起点。
// 复用 IRIS_VALIDATE_SCALAR_AT 宏（需要把宏在这个 TU 内重新定义；它在
// validate_object/array 后面已经 #undef）。这里手写等价分支，避免再展开。
ValidationReport validate_value_root(Cursor& c, const CompiledSchema& schema) noexcept {
    if (schema.types.empty()) {
        // 任意值都接受
        return make_ok(c.pos);
    }
    const TypeMask              allowed        = schema.types[0];
    const std::uint32_t         min_string_len = schema.min_string_len[0];
    const std::uint32_t         max_string_len = schema.max_string_len[0];
    const double                min_dbl        = schema.min_dbl[0];
    const double                max_dbl        = schema.max_dbl[0];
    const CompiledSchema* const obj_sub        = schema.nested_object[0].get();
    const TypeMask              item_type      = schema.array_item_type[0];
    const CompiledSchema* const item_nested    = schema.array_item_nested[0].get();

    if (IRIS_UNLIKELY(c.eof())) return make_err(ValidationError::kInvalidJson, c.pos);
    std::uint8_t head = c.peek();

    switch (head) {
        case '"': {
            if (!(allowed & kTypeString)) return make_err(ValidationError::kTypeMismatch, c.pos);
            std::uint32_t s_off, s_len;
            if (!scan_string(c, s_off, s_len)) return make_err(ValidationError::kInvalidJson, c.pos);
            if (min_string_len > 0 || max_string_len > 0) {
                std::uint32_t cp = ::json_string_codepoints(c.data + s_off, s_len);
                if (cp < min_string_len) return make_err(ValidationError::kStringTooShort, s_off);
                if (max_string_len > 0 && cp > max_string_len)
                    return make_err(ValidationError::kStringTooLong, s_off);
            }
            return make_ok(c.pos);
        }
        case 't': {
            if (!(allowed & kTypeBoolean)) return make_err(ValidationError::kTypeMismatch, c.pos);
            if (!match_true(c.data + c.pos, c.size - c.pos)) return make_err(ValidationError::kInvalidJson, c.pos);
            c.pos += 4; return make_ok(c.pos);
        }
        case 'f': {
            if (!(allowed & kTypeBoolean)) return make_err(ValidationError::kTypeMismatch, c.pos);
            if (!match_false(c.data + c.pos, c.size - c.pos)) return make_err(ValidationError::kInvalidJson, c.pos);
            c.pos += 5; return make_ok(c.pos);
        }
        case 'n': {
            if (!(allowed & kTypeNull)) return make_err(ValidationError::kTypeMismatch, c.pos);
            if (!match_null(c.data + c.pos, c.size - c.pos)) return make_err(ValidationError::kInvalidJson, c.pos);
            c.pos += 4; return make_ok(c.pos);
        }
        case '{': {
            if (!(allowed & kTypeObject)) return make_err(ValidationError::kTypeMismatch, c.pos);
            if (obj_sub) return validate_object(c, *obj_sub);
            if (!skip_balanced(c)) return make_err(ValidationError::kInvalidJson, c.pos);
            return make_ok(c.pos);
        }
        case '[': {
            if (!(allowed & kTypeArray)) return make_err(ValidationError::kTypeMismatch, c.pos);
            if (item_type != 0) return validate_array(c, item_type, item_nested);
            if (!skip_balanced(c)) return make_err(ValidationError::kInvalidJson, c.pos);
            return make_ok(c.pos);
        }
        default: {
            TypeMask vt = token_to_type(head);
            if (vt == kTypeNone) return make_err(ValidationError::kUnexpectedToken, c.pos);
            NumberScan ns = scan_number(c.data, c.size, c.pos);
            if (!ns.is_number) return make_err(ValidationError::kInvalidJson, c.pos);
            if (ns.is_integer) {
                if (!(allowed & (kTypeInteger | kTypeNumber)))
                    return make_err(ValidationError::kTypeMismatch, c.pos);
            } else {
                if (!(allowed & kTypeNumber)) return make_err(ValidationError::kTypeMismatch, c.pos);
            }
            if (ns.dbl_value < min_dbl || ns.dbl_value > max_dbl)
                return make_err(ValidationError::kIntOutOfRange, c.pos);
            c.pos = ns.end_pos;
            return make_ok(c.pos);
        }
    }
}

}  // namespace

ValidationReport validate(const std::uint8_t* data, std::size_t size,
                          const CompiledSchema& schema) noexcept {
    Cursor c{data, size, 0};
    c.skip_ws();
    switch (schema.kind) {
        case SchemaKind::kAlwaysValid:
            return make_ok(c.pos);
        case SchemaKind::kAlwaysInvalid:
            return make_err(ValidationError::kTypeMismatch, c.pos);
        case SchemaKind::kObjectRoot:
            return validate_object(c, schema);
        case SchemaKind::kValueRoot:
            return validate_value_root(c, schema);
    }
    return make_err(ValidationError::kInvalidJson, c.pos);
}

}  // namespace iris
