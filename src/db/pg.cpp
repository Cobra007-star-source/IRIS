// =============================================================================
// src/db/pg.cpp
//
// libpq async wrapper implementation. See iris/db/pg.hpp for the lifecycle
// rationale (blocking connect/prepare at startup, non-blocking query I/O on the
// hot path, binary result decoding).
// =============================================================================
#include "iris/db/pg.hpp"

#include <libpq-fe.h>

#include <cstdint>
#include <cstring>

namespace iris::db {

PgConn::~PgConn() { close(); }

PgConn::PgConn(PgConn&& o) noexcept : conn_(o.conn_) { o.conn_ = nullptr; }

PgConn& PgConn::operator=(PgConn&& o) noexcept {
    if (this != &o) {
        close();
        conn_   = o.conn_;
        o.conn_ = nullptr;
    }
    return *this;
}

void PgConn::close() noexcept {
    if (conn_) {
        ::PQfinish(conn_);
        conn_ = nullptr;
    }
}

bool PgConn::connect(const char* conninfo) noexcept {
    close();
    conn_ = ::PQconnectdb(conninfo);
    if (!conn_ || ::PQstatus(conn_) != CONNECTION_OK) {
        close();
        return false;
    }
    // All query I/O after bring-up is non-blocking; sends may report partial
    // flush, which the event loop resolves via writable readiness.
    if (::PQsetnonblocking(conn_, 1) != 0) {
        close();
        return false;
    }
    return true;
}

bool PgConn::prepare(const char* name, const char* sql, int nparams) noexcept {
    if (!conn_) return false;
    PGresult* r = ::PQprepare(conn_, name, sql, nparams, nullptr);
    const bool ok = r && ::PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) ::PQclear(r);
    return ok;
}

bool PgConn::is_bad() const noexcept {
    return !conn_ || ::PQstatus(conn_) == CONNECTION_BAD;
}

int PgConn::socket() const noexcept {
    return conn_ ? ::PQsocket(conn_) : -1;
}

std::string PgConn::error() const noexcept {
    return conn_ ? ::PQerrorMessage(conn_) : "no connection";
}

bool PgConn::send_prepared(const char* name, int nparams,
                           const char* const* values, const int* lengths,
                           const int* formats, bool result_binary) noexcept {
    if (!conn_) return false;
    return ::PQsendQueryPrepared(conn_, name, nparams, values, lengths, formats,
                                 result_binary ? 1 : 0) == 1;
}

bool PgConn::send_query(const char* sql) noexcept {
    if (!conn_) return false;
    return ::PQsendQuery(conn_, sql) == 1;
}

int PgConn::flush() noexcept {
    if (!conn_) return -1;
    return ::PQflush(conn_);
}

Pump PgConn::on_readable() noexcept {
    if (!conn_) return Pump::kError;
    if (::PQconsumeInput(conn_) == 0) return Pump::kError;
    if (::PQisBusy(conn_) == 1) return Pump::kBusy;
    return Pump::kReady;
}

bool PgConn::consume_input() noexcept {
    return conn_ && ::PQconsumeInput(conn_) == 1;
}

bool PgConn::is_busy() noexcept {
    return conn_ && ::PQisBusy(conn_) == 1;
}

pg_result* PgConn::take_result() noexcept {
    return conn_ ? ::PQgetResult(conn_) : nullptr;
}

bool PgConn::pipeline_enter() noexcept {
    return conn_ && ::PQenterPipelineMode(conn_) == 1;
}

bool PgConn::pipeline_sync() noexcept {
    return conn_ && ::PQpipelineSync(conn_) == 1;
}

bool PgConn::pipeline_exit() noexcept {
    return conn_ && ::PQexitPipelineMode(conn_) == 1;
}

// --- binary result decoding --------------------------------------------------

std::int32_t bin_int4(const pg_result* r, int row, int col) noexcept {
    const char* p = ::PQgetvalue(const_cast<PGresult*>(r), row, col);
    if (!p) return 0;
    // Binary int4 is network byte order (big-endian); assemble explicitly so the
    // decode is endian-independent and branchless.
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::int32_t>((std::uint32_t(b[0]) << 24) |
                                     (std::uint32_t(b[1]) << 16) |
                                     (std::uint32_t(b[2]) << 8) |
                                      std::uint32_t(b[3]));
}

std::string_view text_field(const pg_result* r, int row, int col) noexcept {
    PGresult*   m = const_cast<PGresult*>(r);
    const char* p = ::PQgetvalue(m, row, col);
    const int   n = ::PQgetlength(m, row, col);
    if (!p || n < 0) return {};
    return std::string_view(p, static_cast<std::size_t>(n));
}

int result_rows(const pg_result* r) noexcept {
    return ::PQntuples(const_cast<PGresult*>(r));
}

bool result_ok_tuples(const pg_result* r) noexcept {
    return ::PQresultStatus(const_cast<PGresult*>(r)) == PGRES_TUPLES_OK;
}

bool result_ok_command(const pg_result* r) noexcept {
    return ::PQresultStatus(const_cast<PGresult*>(r)) == PGRES_COMMAND_OK;
}

bool result_is_pipeline_sync(const pg_result* r) noexcept {
    return ::PQresultStatus(const_cast<PGresult*>(r)) == PGRES_PIPELINE_SYNC;
}

bool result_is_error(const pg_result* r) noexcept {
    const ExecStatusType s = ::PQresultStatus(const_cast<PGresult*>(r));
    return s == PGRES_FATAL_ERROR || s == PGRES_NONFATAL_ERROR ||
           s == PGRES_PIPELINE_ABORTED;
}

void clear_result(pg_result* r) noexcept {
    if (r) ::PQclear(r);
}

}  // namespace iris::db
