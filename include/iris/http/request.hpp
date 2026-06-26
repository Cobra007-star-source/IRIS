// =============================================================================
// iris/http/request.hpp
//
// Parsed view of a single HTTP/1.x request. All string_views point into the
// owning connection's read buffer and stay valid only until that buffer is
// compacted, so handlers must consume them synchronously.
// =============================================================================
#pragma once

#include <string_view>

namespace iris::http {

struct Request {
    std::string_view method;          // e.g. "GET"
    std::string_view path;            // e.g. "/plaintext" (may include query)
    std::string_view body;            // request body (empty for GET)
    std::string_view range;           // Range header value (empty if absent)
    std::string_view if_none_match;   // If-None-Match header value
    std::string_view accept_encoding; // Accept-Encoding header value
    int              minor_version = 1;   // HTTP/1.<minor_version>
    bool             keep_alive    = true;
    std::size_t      content_length = 0;  // declared body length (0 if none)
};

}  // namespace iris::http
