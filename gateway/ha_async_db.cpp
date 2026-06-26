// =============================================================================
// gateway/ha_async_db.cpp
// =============================================================================
#include "ha_async_db.hpp"

#include <cstdlib>
#include <cstring>

#include "iris/db/pg.hpp"
#include "iris/http/buffer.hpp"
#include "iris/http/request.hpp"
#include "iris/http/response.hpp"

namespace iris::ha {

namespace {

long parse_qint(std::string_view qs, const char* key, long def) noexcept {
    const std::string needle =
        std::string(key) + "=";
    std::size_t p = qs.find(needle);
    if (p == std::string_view::npos) return def;
    p += needle.size();
    long v = 0;
    bool any = false;
    while (p < qs.size() && qs[p] >= '0' && qs[p] <= '9') {
        v = v * 10 + (qs[p] - '0');
        any = true;
        ++p;
    }
    return any ? v : def;
}

void append_sv(iris::http::Buffer& out, std::string_view s) noexcept {
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:   out.append(c);
        }
    }
}

inline bool pg_bool(std::string_view s) noexcept {
    return !s.empty() && (s[0] == 't' || s[0] == 'T' || s[0] == '1');
}

inline int parse_int_sv(std::string_view sv) noexcept {
    int v = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return v;
}

}  // namespace

void parse_async_db_query(std::string_view path, int& min_price, int& max_price,
                          int& limit) noexcept {
    std::string_view qs;
    const std::size_t q = path.find('?');
    if (q != std::string_view::npos) qs = path.substr(q + 1);

    min_price = static_cast<int>(parse_qint(qs, "min", 10));
    max_price = static_cast<int>(parse_qint(qs, "max", 50));
    limit     = static_cast<int>(parse_qint(qs, "limit", 50));
    if (limit < 1) limit = 1;
    if (limit > 50) limit = 50;
}

int format_async_db_json(char* out, std::size_t cap, const pg_result* r) noexcept {
    iris::http::Buffer buf(out, cap);
    buf.append("{\"items\":[");
    const int rows = iris::db::result_rows(r);
    for (int i = 0; i < rows; ++i) {
        if (i) buf.append(',');
        const int id       = parse_int_sv(iris::db::text_field(r, i, 0));
        const auto name      = iris::db::text_field(r, i, 1);
        const auto category  = iris::db::text_field(r, i, 2);
        const int price      = parse_int_sv(iris::db::text_field(r, i, 3));
        const int quantity   = parse_int_sv(iris::db::text_field(r, i, 4));
        const bool active    = pg_bool(iris::db::text_field(r, i, 5));
        const auto tags      = iris::db::text_field(r, i, 6);
        const int score      = parse_int_sv(iris::db::text_field(r, i, 7));
        const int count      = parse_int_sv(iris::db::text_field(r, i, 8));

        buf.append("{\"id\":");
        buf.append_uint(static_cast<std::size_t>(id));
        buf.append(",\"name\":\"");
        append_sv(buf, name);
        buf.append("\",\"category\":\"");
        append_sv(buf, category);
        buf.append("\",\"price\":");
        buf.append_uint(static_cast<std::size_t>(price));
        buf.append(",\"quantity\":");
        buf.append_uint(static_cast<std::size_t>(quantity));
        buf.append(",\"active\":");
        buf.append(active ? "true" : "false");
        buf.append(",\"tags\":");
        if (tags.empty()) buf.append("[]");
        else buf.append(tags);
        buf.append(",\"rating\":{\"score\":");
        buf.append_uint(static_cast<std::size_t>(score));
        buf.append(",\"count\":");
        buf.append_uint(static_cast<std::size_t>(count));
        buf.append("}}");
    }
    buf.append("],\"count\":");
    buf.append_uint(static_cast<std::size_t>(rows));
    buf.append('}');
    if (buf.overflow()) return -1;
    return static_cast<int>(buf.size());
}

void write_empty_async_db(iris::http::Buffer& out,
                          const iris::http::Request& req) noexcept {
    iris::http::write_response(out, 200, "OK", "application/json",
                               "{\"items\":[],\"count\":0}",
                               req.minor_version, req.keep_alive);
}

}  // namespace iris::ha
