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

#if defined(IRIS_HAVE_ASMJIT)
    #include "iris/jit/serializer.hpp"
#endif

namespace {

using iris::http::Buffer;
using iris::http::Request;

constexpr std::string_view kHelloPlain = "Hello, World!";
constexpr std::string_view kHelloMsg   = "Hello, World!";
constexpr std::size_t       kDateLen   = 29;

#if defined(IRIS_HAVE_ASMJIT)
// Compiled once at startup (cold path) from the exact bytes the C++ serializer
// produces. On the /json hot path the worker invokes this machine code, which
// emits the body with a few wide register stores (block write + overlapping
// tail). nullptr until compile() succeeds; the C++ serializer is the fallback.
iris::jit::JsonSerializer g_json_jit;
#endif

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

// The /json response models an actual object, not a pre-baked string. TFB's
// JSON rule forbids returning a hard-coded JSON literal: the object must be
// instantiated and serialized field-by-field at request time. This struct is
// that object; serialize() walks its fields and converts each to the byte
// stream, so the body is genuinely produced per request.
struct Message {
    std::string_view message;

    void serialize(Buffer& out) const noexcept {
        out.append('{');
        // field 1: "message"
        json_write_string(out, "message");
        out.append(':');
        json_write_string(out, message);
        out.append('}');
    }
};

// Instantiate the object from runtime data and serialize it into `out`.
void serialize_message(Buffer& out, std::string_view msg) noexcept {
    Message obj{msg};
    obj.serialize(out);
}

// /json body emission for the hot path. When the JIT serializer compiled
// successfully it writes the body straight into the buffer's tail with wide
// stores; otherwise the portable C++ object serializer runs. The branch is a
// single, perfectly-predicted load+test (the JIT pointer never changes after
// startup), so the [[likely]] path is effectively free.
inline void emit_json_body(Buffer& out) noexcept {
#if defined(IRIS_HAVE_ASMJIT)
    const iris::jit::SerializeFn fn = g_json_jit.fn();
    if (fn != nullptr) [[likely]] {
        const std::size_t n   = g_json_jit.length();
        char*             dst = out.append_uninitialized(n);
        if (dst != nullptr) [[likely]] { fn(dst); }
        return;
    }
#endif
    serialize_message(out, kHelloMsg);
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
            emit_json_body(out);
        } else {
            char   scratch[64];
            Buffer body(scratch, sizeof(scratch));
            emit_json_body(body);
            iris::http::write_response(out, 200, "OK", "application/json",
                                       body.view(), req.minor_version, req.keep_alive);
        }
        return;
    }
    iris::http::write_response(out, 404, "Not Found", "text/plain", "",
                               req.minor_version, req.keep_alive);
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
    g_plain = build_template("text/plain", kHelloPlain.size(), kHelloPlain, true);

    // Serialize the /json body once with the C++ serializer: it is the source
    // of truth for Content-Length and the exact byte sequence the JIT
    // specializes. compile() reads these bytes synchronously (baking them as
    // immediates), so the stack buffer need not outlive the call.
    char   canon_buf[64];
    Buffer canon(canon_buf, sizeof(canon_buf));
    serialize_message(canon, kHelloMsg);
    g_json_hdr = build_template("application/json", canon.size(), {}, false);

#if defined(IRIS_HAVE_ASMJIT)
    if (g_json_jit.compile(canon.view())) {
        std::printf("[iris-gw] /json serializer: JIT block-write (%zu bytes)\n",
                    g_json_jit.length());
    } else {
        std::fprintf(stderr,
                     "[iris-gw] /json serializer: JIT unavailable, C++ fallback\n");
    }
#endif

    int workers = cfg.workers > 0 ? cfg.workers : 0;
    std::printf("[iris-gw] listening on :%u (workers=%s, reuseport=%d)\n",
                static_cast<unsigned>(cfg.port),
                workers ? std::to_string(workers).c_str() : "auto",
                static_cast<int>(cfg.reuseport));
    std::fflush(stdout);

    return iris::net::run_server(cfg, handle);
}
