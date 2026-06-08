// =============================================================================
// iris/http/response.hpp
//
// Allocation-free HTTP/1.1 response assembly. write_response() appends a full
// response (status line + standard headers + body) into a caller-owned Buffer,
// pulling the Date header from the global 1 Hz clock. Kept header-inline so it
// folds into the route handler.
// =============================================================================
#pragma once

#include <string_view>

#include "iris/http/buffer.hpp"
#include "iris/http/date.hpp"

namespace iris::http {

inline constexpr std::string_view kServerName = "iris";

// Always emits an HTTP/1.1 status line. The Connection header is chosen for
// correctness across protocol versions:
//   * keep_alive == false            -> "Connection: close"
//   * keep_alive, request was 1.0    -> "Connection: keep-alive" (1.0 requires
//                                        it explicitly, else the client waits
//                                        for the server to close)
//   * keep_alive, request was >= 1.1 -> no header (1.1 default)
inline void write_response(Buffer& out, int status, std::string_view reason,
                           std::string_view content_type, std::string_view body,
                           int minor_version, bool keep_alive) noexcept {
    out.append("HTTP/1.1 ");
    out.append_uint(static_cast<std::size_t>(status));
    out.append(' ');
    out.append(reason);
    out.append("\r\nServer: ");
    out.append(kServerName);
    out.append("\r\nDate: ");
    out.append(current_date());
    out.append("\r\nContent-Type: ");
    out.append(content_type);
    out.append("\r\nContent-Length: ");
    out.append_uint(body.size());
    if (!keep_alive) {
        out.append("\r\nConnection: close");
    } else if (minor_version == 0) {
        out.append("\r\nConnection: keep-alive");
    }
    out.append("\r\n\r\n");
    out.append(body);
}

}  // namespace iris::http
