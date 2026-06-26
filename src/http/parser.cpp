// =============================================================================
// src/http/parser.cpp
//
// picohttpparser-backed request parsing. Supports Content-Length bodies for
// WFB POST /json/aggregate; TFB GET routes remain zero-body.
// =============================================================================
#include "iris/http/parser.hpp"

#include <cstdlib>
#include <cstring>    // memcpy
#include <strings.h>  // strncasecmp

#include "picohttpparser.h"

namespace iris::http {

namespace {

inline bool iequals(const char* a, std::size_t alen, const char* lower,
                    std::size_t llen) noexcept {
    return alen == llen && ::strncasecmp(a, lower, llen) == 0;
}

inline bool parse_content_length(const char* v, std::size_t vl,
                                 std::size_t& out) noexcept {
    if (vl == 0) return false;
    std::size_t n = 0;
    for (std::size_t i = 0; i < vl; ++i) {
        const char c = v[i];
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    out = n;
    return true;
}

inline bool has_chunked(const char* v, std::size_t vl) noexcept {
    // "chunked" is the last (only) coding for the workloads we serve.
    for (std::size_t i = 0; i + 7 <= vl; ++i) {
        if (::strncasecmp(v + i, "chunked", 7) == 0) return true;
    }
    return false;
}

// Decode a chunked body starting at buf[0..len) into `scratch`. Returns the
// number of raw framing bytes consumed (>0) on success, 0 if more bytes are
// needed, SIZE_MAX on malformed framing or scratch overflow.
inline std::size_t decode_chunked(const char* buf, std::size_t len, char* scratch,
                                  std::size_t scratch_cap,
                                  std::size_t& decoded_len) noexcept {
    std::size_t pos = 0, out = 0;
    for (;;) {
        std::size_t size = 0;
        bool any = false;
        while (pos < len) {
            const char c = buf[pos];
            std::size_t d;
            if (c >= '0' && c <= '9')      d = static_cast<std::size_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<std::size_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<std::size_t>(c - 'A' + 10);
            else break;
            size = size * 16 + d;
            any = true;
            ++pos;
        }
        if (pos >= len) return 0;            // need the size line's CRLF
        if (!any) return static_cast<std::size_t>(-1);
        // Skip chunk extensions (";...") up to CRLF.
        while (pos < len && buf[pos] != '\r') ++pos;
        if (pos + 1 >= len) return 0;
        if (buf[pos] != '\r' || buf[pos + 1] != '\n') return static_cast<std::size_t>(-1);
        pos += 2;
        if (size == 0) {                     // terminating chunk + trailing CRLF
            if (pos + 1 >= len) return 0;
            if (buf[pos] != '\r' || buf[pos + 1] != '\n') return static_cast<std::size_t>(-1);
            decoded_len = out;
            return pos + 2;
        }
        if (pos + size + 2 > len) return 0;  // need data + trailing CRLF
        if (out + size > scratch_cap) return static_cast<std::size_t>(-1);
        std::memcpy(scratch + out, buf + pos, size);
        out += size;
        pos += size;
        if (buf[pos] != '\r' || buf[pos + 1] != '\n') return static_cast<std::size_t>(-1);
        pos += 2;
    }
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
    out.content_length = 0;
    out.body          = {};
    out.range         = {};
    out.if_none_match = {};
    out.accept_encoding = {};

    bool keep_alive = (minor_version >= 1);
    std::size_t content_length = 0;
    bool        has_cl           = false;
    bool        chunked          = false;

    for (std::size_t i = 0; i < num_headers; ++i) {
        if (iequals(headers[i].name, headers[i].name_len, "connection", 10)) {
            const char* v = headers[i].value;
            std::size_t vl = headers[i].value_len;
            if (iequals(v, vl, "close", 5))            keep_alive = false;
            else if (iequals(v, vl, "keep-alive", 10)) keep_alive = true;
        } else if (iequals(headers[i].name, headers[i].name_len,
                           "content-length", 14)) {
            if (parse_content_length(headers[i].value, headers[i].value_len,
                                     content_length)) {
                has_cl = true;
            }
        } else if (iequals(headers[i].name, headers[i].name_len,
                           "transfer-encoding", 17)) {
            if (has_chunked(headers[i].value, headers[i].value_len)) chunked = true;
        } else if (iequals(headers[i].name, headers[i].name_len, "range", 5)) {
            out.range = std::string_view(headers[i].value, headers[i].value_len);
        } else if (iequals(headers[i].name, headers[i].name_len,
                           "if-none-match", 13)) {
            out.if_none_match =
                std::string_view(headers[i].value, headers[i].value_len);
        } else if (iequals(headers[i].name, headers[i].name_len,
                           "accept-encoding", 15)) {
            out.accept_encoding =
                std::string_view(headers[i].value, headers[i].value_len);
        }
    }
    out.keep_alive     = keep_alive;
    out.content_length = content_length;

    const std::size_t hdr_end = static_cast<std::size_t>(pret);
    if (chunked) {
        // Decode into a per-thread scratch; the body view stays valid for the
        // synchronous handler call (drain() consumes it before the next parse).
        static thread_local char scratch[65536];
        std::size_t decoded = 0;
        const std::size_t framed =
            decode_chunked(buf + hdr_end, len - hdr_end, scratch, sizeof(scratch),
                           decoded);
        if (framed == 0) return {ParseStatus::kIncomplete, 0};
        if (framed == static_cast<std::size_t>(-1)) return {ParseStatus::kError, 0};
        out.body           = std::string_view(scratch, decoded);
        out.content_length = decoded;
        return {ParseStatus::kOk, hdr_end + framed};
    }
    if (has_cl) {
        if (len < hdr_end + content_length) return {ParseStatus::kIncomplete, 0};
        out.body = std::string_view(buf + hdr_end, content_length);
        return {ParseStatus::kOk, hdr_end + content_length};
    }

    return {ParseStatus::kOk, hdr_end};
}

}  // namespace iris::http
