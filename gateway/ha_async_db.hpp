// =============================================================================
// gateway/ha_async_db.hpp
//
// HTTPArena `async-db` profile: GET /async-db?min=&max=&limit= with async
// Postgres and per-request JSON serialization.
// =============================================================================
#pragma once

#include <string_view>

struct pg_result;

namespace iris::http {
class Buffer;
class Request;
}

namespace iris::ha {

// Parse query params (defaults min=10 max=50 limit=50; limit clamped 1..50).
void parse_async_db_query(std::string_view path, int& min_price, int& max_price,
                          int& limit) noexcept;

// Serialize query rows into HttpArena JSON shape. Returns body length or -1.
int format_async_db_json(char* out, std::size_t cap, const pg_result* r) noexcept;

void write_empty_async_db(iris::http::Buffer& out, const iris::http::Request& req) noexcept;

}  // namespace iris::ha
