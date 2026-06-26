// =============================================================================
// iris/net/server.hpp
//
// Thread-per-core HTTP/1.1 server core. Each worker owns a poller, its own
// SO_REUSEPORT listener (racing profile), a pool of per-connection fixed
// buffers, and (when configured) a pool of async PostgreSQL connections whose
// sockets share the same poller. The steady state is allocation-free and
// lock-free across cores.
//
// Routing is supplied by the caller as a Handler. Sync routes (/plaintext,
// /json) write a complete response and return kResponded. DB routes call
// AsyncCtx::run_db() and return kSuspended; the core finishes the request when
// the database result arrives, then resumes the connection. The transport,
// pooling, and async plumbing live here; the gateway owns route dispatch and
// the body formatters.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "iris/http/buffer.hpp"
#include "iris/http/request.hpp"

namespace iris::net {

// Outcome of a handler invocation.
enum class Outcome : std::uint8_t {
    kResponded = 0,  // a full response was written into `out`
    kSuspended = 1,  // an async DB op was started; the core will finish later
};

// DB routes the gateway can ask the core to run asynchronously. The World id
// range and query shapes are TFB-fixed; the core owns id generation and the
// libpq state machine, the gateway owns the body formatting.
enum class DbRoute : std::uint8_t {
    kWorldOne,     // /db        : one random World row
    kWorldMany,    // /queries   : N random World rows
    kWorldUpdate,  // /updates   : N random World rows, then bulk UPDATE
    kFortunes,     // /fortunes  : all Fortune rows + one appended, sorted
#if defined(IRIS_WFB)
    kUserProfile,  // WFB /db/user-profile/:email
#endif
};

// A decoded Fortune row. `message` points into the live PGresult and is only
// valid for the duration of the formatter call.
struct FortuneRow {
    std::int32_t     id;
    std::string_view message;
};

// Body formatters supplied by the gateway. The core assembles the status line,
// Date, Content-Type and Content-Length, then calls the matching formatter to
// write only the response body.
struct DbFormatters {
    void (*world_one)(iris::http::Buffer& out, std::int32_t id,
                      std::int32_t rn) = nullptr;
    void (*world_many)(iris::http::Buffer& out, const std::int32_t* ids,
                       const std::int32_t* rns, int n) = nullptr;
    void (*fortunes)(iris::http::Buffer& out, const FortuneRow* rows,
                     int n) = nullptr;
};

// Async context handed to the handler. Sync routes ignore it. DB routes call
// run_db() and return Outcome::kSuspended.
class AsyncCtx {
public:
    // Acquire a pooled DB connection and start `route`. `count` is the number of
    // World queries for kWorldMany / kWorldUpdate (clamped to [1,500] by the
    // core); ignored for kWorldOne / kFortunes. Returns false when DB is
    // disabled or every pooled connection is busy -- the handler must then write
    // an error response and return Outcome::kResponded.
    [[nodiscard]] bool run_db(DbRoute route, int count) noexcept;

#if defined(IRIS_WFB)
    // WFB db_complex: fetch user profile by email (copied into the job).
    [[nodiscard]] bool run_db_profile(std::string_view email) noexcept;
#endif

    // Tier 1 zero-copy: register an immutable body sent after header bytes via
    // writev. Pointer must stay valid for the process lifetime.
    void set_zerocopy_body(const char* data, std::size_t len) noexcept;

    // Tier 2 frozen response: send [offset, offset+len) from a sealed memfd via
    // sendfile with no user-space copy. Mutually exclusive with set_zerocopy_body.
    void set_sendfile_response(int fd, std::size_t offset, std::size_t len) noexcept;

    // Set by the core immediately before each handler call (opaque worker /
    // connection pointers; defined in src/net/server.cpp).
    void* worker_ = nullptr;
    void* conn_   = nullptr;
};

// Route callback. Inspect `req`; either write a complete response into `out`
// (return kResponded) or start an async DB op via `ctx` (return kSuspended).
// Must not retain `req`'s string_views past the call.
using Handler = Outcome (*)(const iris::http::Request& req,
                            iris::http::Buffer& out, AsyncCtx& ctx);

// Per-worker async PostgreSQL pool configuration. conninfo == nullptr disables
// DB support entirely (the gateway then 404s the DB routes).
//
// IMPORTANT: pool_per_worker * workers must stay <= the server's
// max_connections, or bring-up fails for the workers that cannot connect.
struct DbConfig {
    const char*  conninfo        = nullptr;
    int          pool_per_worker = 8;
    DbFormatters fmt{};
};

struct ServerConfig {
    std::uint16_t port         = 8080;
    int           workers      = 0;      // 0 => hardware_concurrency()
    bool          reuseport    = true;   // one listener per worker
    bool          pin_threads  = true;   // pin worker i to CPU i (Linux)
    int           backlog      = 1024;
    std::size_t   read_cap     = 4096;   // per-connection read buffer
    std::size_t   write_cap    = 32768;  // per-connection write buffer (pipelining)
    DbConfig      db{};                  // optional async DB pool
};

// Spawn the workers and run until a fatal error. Blocks the calling thread.
// Returns non-zero on startup failure.
int run_server(const ServerConfig& cfg, Handler handler) noexcept;

}  // namespace iris::net
