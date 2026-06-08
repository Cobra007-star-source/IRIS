// =============================================================================
// iris/net/server.hpp
//
// Thread-per-core HTTP/1.1 server core. Each worker owns a poller, its own
// SO_REUSEPORT listener (racing profile), and a pool of per-connection fixed
// buffers, so the steady state is allocation-free and lock-free across cores.
//
// The server is transport + framing only; routing is supplied by the caller as
// a Handler. The gateway (gateway/main.cpp) provides the TFB routes.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "iris/http/buffer.hpp"
#include "iris/http/request.hpp"

namespace iris::net {

// Route callback: inspect `req` and append a complete HTTP response into `out`.
// Must be allocation-free and must not retain `req`'s string_views.
using Handler = void (*)(const iris::http::Request& req, iris::http::Buffer& out);

struct ServerConfig {
    std::uint16_t port         = 8080;
    int           workers      = 0;      // 0 => hardware_concurrency()
    bool          reuseport    = true;   // one listener per worker
    bool          pin_threads  = true;   // pin worker i to CPU i (Linux)
    int           backlog      = 1024;
    std::size_t   read_cap     = 4096;   // per-connection read buffer
    std::size_t   write_cap    = 32768;  // per-connection write buffer (pipelining)
};

// Spawn the workers and run until a fatal error. Blocks the calling thread.
// Returns non-zero on startup failure.
int run_server(const ServerConfig& cfg, Handler handler) noexcept;

}  // namespace iris::net
