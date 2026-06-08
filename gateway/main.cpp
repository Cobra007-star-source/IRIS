// =============================================================================
// gateway/main.cpp
//
// IRIS gateway entry point. Wires the TechEmpower network-bound routes onto the
// thread-per-core server core:
//
//   GET /plaintext -> "Hello, World!"             (text/plain)
//   GET /json      -> {"message":"Hello, World!"} (application/json)
//
// Hot-path strategy (TFB-grade): the static parts of each response are
// precomputed once at startup into a byte template with a fixed-offset Date
// slot. Per request the worker memcpy's the template into its own write buffer
// and overwrites just the 29-byte Date (no per-request status-line / header
// formatting, no allocation, no shared mutable state). For /json the headers
// are templated while the body is re-serialized each request by a real JSON
// serializer, honoring the TFB rule that the object be serialized per request.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "iris/http/buffer.hpp"
#include "iris/http/date.hpp"
#include "iris/http/response.hpp"
#include "iris/net/server.hpp"

namespace {

using iris::http::Buffer;
using iris::http::Request;

constexpr std::string_view kHelloPlain = "Hello, World!";
constexpr std::string_view kHelloMsg   = "Hello, World!";
constexpr std::size_t       kDateLen   = 29;

// ---- minimal JSON serializer -------------------------------------------------

// Write a quoted, RFC 8259-escaped JSON string.
void json_write_string(Buffer& out, std::string_view s) noexcept {
    out.append('"');
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out.append("\\u00");
                    out.append(hex[(c >> 4) & 0xF]);
                    out.append(hex[c & 0xF]);
                } else {
                    out.append(c);
                }
        }
    }
    out.append('"');
}

// Serialize {"message": <msg>}.
void serialize_message(Buffer& out, std::string_view msg) noexcept {
    out.append("{\"message\":");
    json_write_string(out, msg);
    out.append('}');
}

// ---- precomputed response templates -----------------------------------------

struct RespTemplate {
    std::string bytes;       // full prefix incl. Date placeholder
    std::size_t date_off = 0;  // byte offset of the 29-char Date value
};

// Build "HTTP/1.1 200 OK\r\n... Date: <29 chars> ...\r\n\r\n[body]".
// If include_body is false, the template ends after the header terminator.
RespTemplate build_template(std::string_view content_type,
                            std::size_t content_length, std::string_view body,
                            bool include_body) {
    RespTemplate t;
    std::string& b = t.bytes;
    b  = "HTTP/1.1 200 OK\r\nServer: ";
    b += iris::http::kServerName;
    b += "\r\nDate: ";
    t.date_off = b.size();
    b.append(kDateLen, '?');  // placeholder, overwritten per request
    b += "\r\nContent-Type: ";
    b += content_type;
    b += "\r\nContent-Length: ";
    b += std::to_string(content_length);
    b += "\r\n\r\n";
    if (include_body) b.append(body.data(), body.size());
    return t;
}

RespTemplate g_plain;     // full response (headers + body)
RespTemplate g_json_hdr;  // headers only; body serialized per request

// Copy a template into `out` and patch its Date slot with the current value.
inline void emit_template(Buffer& out, const RespTemplate& t) noexcept {
    const std::size_t base = out.size();
    out.append(std::string_view(t.bytes));
    if (!out.overflow()) {
        std::memcpy(out.data() + base + t.date_off, iris::http::current_date().data(),
                    kDateLen);
    }
}

// The templated fast path is a pure HTTP/1.1 keep-alive response (no Connection
// header). It is taken only for HTTP/1.1 keep-alive traffic, which is ~100% of
// TFB load (wrk speaks 1.1). HTTP/1.0 and Connection: close fall back to the
// version-aware generic writer so the Connection header is always correct.
inline bool fast_path(const Request& req) noexcept {
    return req.keep_alive && req.minor_version >= 1;
}

void handle(const Request& req, Buffer& out) {
    if (req.path == "/plaintext") {
        if (fast_path(req)) {
            emit_template(out, g_plain);
        } else {
            iris::http::write_response(out, 200, "OK", "text/plain", kHelloPlain,
                                       req.minor_version, req.keep_alive);
        }
        return;
    }
    if (req.path == "/json") {
        if (fast_path(req)) {
            emit_template(out, g_json_hdr);
            serialize_message(out, kHelloMsg);
        } else {
            char   scratch[64];
            Buffer body(scratch, sizeof(scratch));
            serialize_message(body, kHelloMsg);
            iris::http::write_response(out, 200, "OK", "application/json",
                                       body.view(), req.minor_version, req.keep_alive);
        }
        return;
    }
    iris::http::write_response(out, 404, "Not Found", "text/plain", "",
                               req.minor_version, req.keep_alive);
}

// Compute the serialized /json body length once so the template's
// Content-Length matches exactly.
std::size_t json_body_length() {
    char   scratch[64];
    Buffer b(scratch, sizeof(scratch));
    serialize_message(b, kHelloMsg);
    return b.size();
}

}  // namespace

int main(int argc, char** argv) {
    iris::net::ServerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            cfg.workers = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-pin") == 0) {
            cfg.pin_threads = false;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: iris-gw [--port N] [--workers N] [--no-pin]\n");
            return 0;
        }
    }

    iris::http::start_date_clock();
    g_plain    = build_template("text/plain", kHelloPlain.size(), kHelloPlain, true);
    g_json_hdr = build_template("application/json", json_body_length(), {}, false);

    int workers = cfg.workers > 0 ? cfg.workers : 0;
    std::printf("[iris-gw] listening on :%u (workers=%s, reuseport=%d)\n",
                static_cast<unsigned>(cfg.port),
                workers ? std::to_string(workers).c_str() : "auto",
                static_cast<int>(cfg.reuseport));
    std::fflush(stdout);

    return iris::net::run_server(cfg, handle);
}
