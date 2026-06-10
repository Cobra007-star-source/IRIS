// =============================================================================
// iris/db/pg.hpp
//
// Thin async wrapper over libpq, sized for the TechEmpower database tracks.
//
// Lifecycle split (the key to keeping the hot path simple):
//
//   * Startup (cold path): connect and PREPARE synchronously. Connections are
//     long-lived and pooled per worker, so a blocking PQconnectdb / PQprepare
//     at bring-up costs nothing at steady state and avoids the PQconnectPoll
//     state machine entirely.
//
//   * Per request (hot path): fully non-blocking. PQsendQueryPrepared, then the
//     worker's poller waits for read-readiness, PQconsumeInput drains the
//     socket, and PQgetResult is called once PQisBusy() clears.
//
// Results are requested in BINARY format: World.id / World.randomNumber arrive
// as 4-byte big-endian ints decoded with a single bswap, skipping the text
// atoi() the default format would force.
//
// One PgConn is owned by exactly one worker thread; it carries no locking.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// libpq's header is included only in the implementation TU to keep PG types out
// of the gateway's translation units. PGconn is forward-declared as opaque.
struct pg_conn;
struct pg_result;

namespace iris::db {

// Result of pumping the non-blocking query state machine after readability.
enum class Pump : std::uint8_t {
    kBusy,   // more input needed; keep waiting on the socket
    kReady,  // a result is available via take_result()
    kError,  // connection-level failure; the conn must be recycled
};

class PgConn {
public:
    PgConn() = default;
    ~PgConn();
    PgConn(const PgConn&)            = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&&) noexcept;
    PgConn& operator=(PgConn&&) noexcept;

    // --- startup (blocking) ---------------------------------------------------

    // Open the connection (blocking) and switch the handle to non-blocking mode
    // for all subsequent query I/O. Returns false on failure.
    [[nodiscard]] bool connect(const char* conninfo) noexcept;

    // Register a prepared statement (blocking; startup only). `nparams` is the
    // number of $N placeholders. Returns false on failure.
    [[nodiscard]] bool prepare(const char* name, const char* sql,
                               int nparams) noexcept;

    [[nodiscard]] bool        ok()     const noexcept { return conn_ != nullptr; }
    [[nodiscard]] bool        is_bad() const noexcept;  // PQstatus == CONNECTION_BAD
    [[nodiscard]] int         socket() const noexcept;  // PQsocket, -1 if dead
    [[nodiscard]] std::string error()  const noexcept;  // PQerrorMessage

    // --- hot path (non-blocking) ---------------------------------------------

    // Send a prepared query. `values`/`lengths`/`formats` follow libpq's
    // PQsendQueryPrepared contract; result_binary requests binary rows.
    // Returns false if the send could not be dispatched (recycle the conn).
    [[nodiscard]] bool send_prepared(const char* name, int nparams,
                                     const char* const* values,
                                     const int* lengths, const int* formats,
                                     bool result_binary) noexcept;

    // Send raw SQL (text protocol, no params). Used for bulk UPDATE statements
    // assembled at request time. Returns false on dispatch failure.
    [[nodiscard]] bool send_query(const char* sql) noexcept;

    // Flush buffered output. Returns: 0 = fully sent, 1 = more pending (wait for
    // writable), -1 = error. Call after a send when the socket may be full.
    [[nodiscard]] int flush() noexcept;

    // Socket became readable: consume input and report whether a result is
    // ready. After kReady, call take_result() (possibly repeatedly) until it
    // returns nullptr, which also clears the busy state for the next query.
    [[nodiscard]] Pump on_readable() noexcept;

    // Lower-level primitives for the pipeline result loop. consume_input()
    // drains the socket once (false on error); is_busy() reports whether the
    // next take_result() would block (results not yet fully arrived).
    [[nodiscard]] bool consume_input() noexcept;
    [[nodiscard]] bool is_busy() noexcept;

    // Retrieve the next result (caller owns it; call clear_result on it). Returns
    // nullptr when the current command's results are exhausted.
    [[nodiscard]] pg_result* take_result() noexcept;

    // Enter / check libpq pipeline mode (PG14+). Used by the /queries and
    // /updates tracks to batch round-trips.
    [[nodiscard]] bool pipeline_enter() noexcept;
    [[nodiscard]] bool pipeline_sync()  noexcept;
    [[nodiscard]] bool pipeline_exit()  noexcept;

    [[nodiscard]] pg_conn* raw() const noexcept { return conn_; }

private:
    void close() noexcept;
    pg_conn* conn_ = nullptr;
};

// --- startup cache load (blocking, cold path) ---------------------------------
// Fill `rns` (indexed by id-1, capacity `cap`) from the CachedWorld table,
// falling back to World when CachedWorld does not exist (dev databases).
// Returns the number of rows loaded, or -1 on connection/query failure.
[[nodiscard]] int fetch_world_cache(const char* conninfo, std::int32_t* rns,
                                    int cap) noexcept;

// --- binary result decoding --------------------------------------------------
// libpq returns binary int4 as big-endian. These helpers decode a single cell.
[[nodiscard]] std::int32_t bin_int4(const pg_result* r, int row, int col) noexcept;
[[nodiscard]] std::string_view text_field(const pg_result* r, int row,
                                           int col) noexcept;
[[nodiscard]] int  result_rows(const pg_result* r) noexcept;
[[nodiscard]] bool result_ok_tuples(const pg_result* r) noexcept;  // PGRES_TUPLES_OK
[[nodiscard]] bool result_ok_command(const pg_result* r) noexcept; // PGRES_COMMAND_OK
[[nodiscard]] bool result_is_pipeline_sync(const pg_result* r) noexcept;
[[nodiscard]] bool result_is_error(const pg_result* r) noexcept;   // FATAL_ERROR / ABORTED
void clear_result(pg_result* r) noexcept;

}  // namespace iris::db
