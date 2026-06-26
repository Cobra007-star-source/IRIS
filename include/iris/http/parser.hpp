// =============================================================================
// iris/http/parser.hpp
//
// Thin wrapper over picohttpparser. Parses exactly one request from the front
// of a byte span (the rest of the span may hold further pipelined requests).
// =============================================================================
#pragma once

#include <cstddef>

#include "iris/http/request.hpp"

namespace iris::http {

enum class ParseStatus {
    kOk,          // one full request parsed; `consumed` bytes used
    kIncomplete,  // need more bytes before the request is complete
    kError,       // malformed request line / headers
};

struct ParseResult {
    ParseStatus status   = ParseStatus::kIncomplete;
    std::size_t consumed = 0;  // header + body bytes when status == kOk
};

// Parse one request starting at buf[0..len). Fills `out` on success.
// When Content-Length is present, returns kIncomplete until the full body
// has arrived; then `consumed` covers headers + body and `out.body` is set.
[[nodiscard]] ParseResult parse_request(const char* buf, std::size_t len,
                                        Request& out) noexcept;

}  // namespace iris::http
