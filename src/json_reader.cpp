// =============================================================================
// json_reader.cpp
// Recursive-descent JSON parser (off the hot path)
// =============================================================================
#include "iris/json_reader.hpp"

#include <cctype>
#include <charconv>
#include <cstring>
#include <sstream>

namespace iris {

namespace {

struct Reader {
    const char* p;
    const char* end;

    bool eof() const noexcept { return p >= end; }
    char peek() const noexcept { return *p; }

    void skip_ws() noexcept {
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
            else break;
        }
    }

    std::size_t offset(const char* base) const noexcept {
        return static_cast<std::size_t>(p - base);
    }
};

struct ParserCtx {
    const char* base;
    JsonParseResult result;

    JsonValue fail(Reader& r, const char* msg) {
        result.diagnostic = msg;
        result.error_offset = r.offset(base);
        result.ok = false;
        return JsonValue::make_null();
    }
};

JsonValue parse_value(Reader& r, ParserCtx& ctx);

JsonValue parse_string(Reader& r, ParserCtx& ctx) {
    ++r.p;  // skip opening "
    std::string out;
    out.reserve(16);
    while (r.p < r.end) {
        char c = *r.p;
        if (c == '"') { ++r.p; return JsonValue::make_string(std::move(out)); }
        if (c == '\\') {
            ++r.p;
            if (r.p >= r.end) return ctx.fail(r, "unterminated escape");
            char esc = *r.p++;
            switch (esc) {
                case '"': out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/');  break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto parse4 = [&](unsigned& out_cp) -> bool {
                        if (r.p + 4 > r.end) return false;
                        unsigned v = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = r.p[i];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= (h - '0');
                            else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
                            else return false;
                        }
                        r.p += 4;
                        out_cp = v;
                        return true;
                    };
                    unsigned cp = 0;
                    if (!parse4(cp)) return ctx.fail(r, "bad \\u escape");
                    // UTF-16 surrogate pair: high surrogate (D800-DBFF)
                    // followed by \uDC00-DFFF → merge into one codepoint
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (r.p + 6 <= r.end && r.p[0] == '\\' && r.p[1] == 'u') {
                            r.p += 2;
                            unsigned low = 0;
                            if (!parse4(low)) return ctx.fail(r, "bad \\u escape after high surrogate");
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                // lone surrogate — spec forbids; use U+FFFD
                                cp = 0xFFFD;
                            }
                        }
                        // dangling high surrogate: encode as U+FFFD
                        else cp = 0xFFFD;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        // dangling low surrogate
                        cp = 0xFFFD;
                    }
                    // encode as UTF-8 (1-4 bytes)
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return ctx.fail(r, "bad escape char");
            }
        } else {
            out.push_back(c);
            ++r.p;
        }
    }
    return ctx.fail(r, "unterminated string");
}

JsonValue parse_number(Reader& r, ParserCtx& ctx) {
    const char* start = r.p;
    if (*r.p == '-') ++r.p;
    while (r.p < r.end && *r.p >= '0' && *r.p <= '9') ++r.p;
    bool is_double = false;
    if (r.p < r.end && *r.p == '.') {
        is_double = true;
        ++r.p;
        while (r.p < r.end && *r.p >= '0' && *r.p <= '9') ++r.p;
    }
    if (r.p < r.end && (*r.p == 'e' || *r.p == 'E')) {
        is_double = true;
        ++r.p;
        if (r.p < r.end && (*r.p == '+' || *r.p == '-')) ++r.p;
        while (r.p < r.end && *r.p >= '0' && *r.p <= '9') ++r.p;
    }
    if (start == r.p) return ctx.fail(r, "expected number");

    if (is_double) {
        double d = 0.0;
        auto res = std::from_chars(start, r.p, d);
        if (res.ec != std::errc{}) return ctx.fail(r, "bad number");
        return JsonValue::make_double(d);
    }
    std::int64_t i = 0;
    auto res = std::from_chars(start, r.p, i);
    if (res.ec != std::errc{}) return ctx.fail(r, "bad integer");
    return JsonValue::make_int(i);
}

JsonValue parse_array(Reader& r, ParserCtx& ctx) {
    ++r.p;  // [
    r.skip_ws();
    JsonArray arr;
    if (!r.eof() && r.peek() == ']') { ++r.p; return JsonValue::make_array(std::move(arr)); }
    while (true) {
        r.skip_ws();
        JsonValue v = parse_value(r, ctx);
        if (!ctx.result.ok) return JsonValue::make_null();
        arr.push_back(std::move(v));
        r.skip_ws();
        if (r.eof()) return ctx.fail(r, "unterminated array");
        char c = r.peek();
        if (c == ',') { ++r.p; continue; }
        if (c == ']') { ++r.p; return JsonValue::make_array(std::move(arr)); }
        return ctx.fail(r, "expected , or ] in array");
    }
}

JsonValue parse_object(Reader& r, ParserCtx& ctx) {
    ++r.p;  // {
    r.skip_ws();
    JsonObject obj;
    if (!r.eof() && r.peek() == '}') { ++r.p; return JsonValue::make_object(std::move(obj)); }
    while (true) {
        r.skip_ws();
        if (r.eof() || r.peek() != '"') return ctx.fail(r, "expected key string");
        JsonValue key = parse_string(r, ctx);
        if (!ctx.result.ok) return JsonValue::make_null();
        r.skip_ws();
        if (r.eof() || r.peek() != ':') return ctx.fail(r, "expected : after key");
        ++r.p;
        r.skip_ws();
        JsonValue val = parse_value(r, ctx);
        if (!ctx.result.ok) return JsonValue::make_null();
        obj.emplace_back(std::move(const_cast<std::string&>(key.as_string())), std::move(val));
        r.skip_ws();
        if (r.eof()) return ctx.fail(r, "unterminated object");
        char c = r.peek();
        if (c == ',') { ++r.p; continue; }
        if (c == '}') { ++r.p; return JsonValue::make_object(std::move(obj)); }
        return ctx.fail(r, "expected , or } in object");
    }
}

JsonValue parse_value(Reader& r, ParserCtx& ctx) {
    if (r.eof()) return ctx.fail(r, "unexpected eof");
    char c = r.peek();
    if (c == '{') return parse_object(r, ctx);
    if (c == '[') return parse_array(r, ctx);
    if (c == '"') return parse_string(r, ctx);
    if (c == 't') {
        if (r.end - r.p < 4 || std::memcmp(r.p, "true", 4) != 0)
            return ctx.fail(r, "expected true");
        r.p += 4;
        return JsonValue::make_bool(true);
    }
    if (c == 'f') {
        if (r.end - r.p < 5 || std::memcmp(r.p, "false", 5) != 0)
            return ctx.fail(r, "expected false");
        r.p += 5;
        return JsonValue::make_bool(false);
    }
    if (c == 'n') {
        if (r.end - r.p < 4 || std::memcmp(r.p, "null", 4) != 0)
            return ctx.fail(r, "expected null");
        r.p += 4;
        return JsonValue::make_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(r, ctx);
    return ctx.fail(r, "unexpected character");
}

}  // namespace

JsonParseResult parse_json(std::string_view text) {
    Reader r{text.data(), text.data() + text.size()};
    ParserCtx ctx{text.data(), {}};
    ctx.result.ok = true;
    r.skip_ws();
    if (r.eof()) {
        ctx.result.ok = false;
        ctx.result.diagnostic = "empty input";
        return std::move(ctx.result);
    }
    JsonValue v = parse_value(r, ctx);
    if (!ctx.result.ok) return std::move(ctx.result);
    r.skip_ws();
    if (!r.eof()) {
        ctx.result.ok = false;
        ctx.result.diagnostic = "trailing garbage";
        ctx.result.error_offset = r.offset(text.data());
        return std::move(ctx.result);
    }
    ctx.result.value = std::move(v);
    return std::move(ctx.result);
}

}  // namespace iris
