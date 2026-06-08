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
    int              minor_version = 1;   // HTTP/1.<minor_version>
    bool             keep_alive    = true;
};

}  // namespace iris::http
