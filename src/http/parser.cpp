// =============================================================================
// src/http/parser.cpp
//
// picohttpparser-backed request parsing. The TFB routes are all GET with no
// body, so a successful parse consumes exactly the header block.
// =============================================================================
#include "iris/http/parser.hpp"

#include <strings.h>  // strncasecmp

#include "picohttpparser.h"

namespace iris::http {

namespace {

// Case-insensitive equality against a lowercase literal of known length.
inline bool iequals(const char* a, std::size_t alen, const char* lower,
                    std::size_t llen) noexcept {
    return alen == llen && ::strncasecmp(a, lower, llen) == 0;
}

}  // namespace

ParseResult parse_request(const char* buf, std::size_t len, Request& out) noexcept {
    const char*       method   = nullptr;
    std::size_t       method_len = 0;
    const char*       path     = nullptr;
    std::size_t       path_len = 0;
    int               minor_version = 0;
    struct phr_header headers[32];
    std::size_t       num_headers = sizeof(headers) / sizeof(headers[0]);

    int pret = phr_parse_request(buf, len, &method, &method_len, &path, &path_len,
                                 &minor_version, headers, &num_headers,
                                 /*last_len=*/0);

    if (pret == -2) return {ParseStatus::kIncomplete, 0};
    if (pret == -1) return {ParseStatus::kError, 0};

    out.method        = std::string_view(method, method_len);
    out.path          = std::string_view(path, path_len);
    out.minor_version = minor_version;

    // HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close. A Connection
    // header overrides the default.
    bool keep_alive = (minor_version >= 1);
    for (std::size_t i = 0; i < num_headers; ++i) {
        if (iequals(headers[i].name, headers[i].name_len, "connection", 10)) {
            const char* v = headers[i].value;
            std::size_t vl = headers[i].value_len;
            if (iequals(v, vl, "close", 5))            keep_alive = false;
            else if (iequals(v, vl, "keep-alive", 10)) keep_alive = true;
        }
    }
    out.keep_alive = keep_alive;

    return {ParseStatus::kOk, static_cast<std::size_t>(pret)};
}

}  // namespace iris::http
