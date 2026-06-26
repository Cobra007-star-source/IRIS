// =============================================================================
// src/net/server.cpp
//
// Thread-per-core HTTP/1.1 event loop with an optional async PostgreSQL engine.
//
//   * One worker per core; each owns a Poller, a SO_REUSEPORT listener (racing
//     profile), a pool of Connections with fixed read/write buffers, and -- when
//     DB is configured -- a pool of long-lived libpq connections whose sockets
//     are registered in the same poller.
//   * Level-triggered readiness. Sync routes (/plaintext, /json) parse every
//     pipelined request and batch responses into one write buffer.
//   * Async DB routes (/db, /queries, /updates, /fortunes) suspend the HTTP
//     connection, issue a non-blocking query on a pooled DB connection, and are
//     completed when that DB socket becomes readable -- then the HTTP connection
//     resumes. Connection bring-up and PREPARE happen once at startup (blocking,
//     cold path); only the per-request round-trip is async.
//   * Zero allocation in steady state: HTTP connections and their buffers are
//     recycled through a per-worker free list; DB connections and their job
//     accumulators are fixed pools.
// =============================================================================
#include "iris/net/server.hpp"

#include "iris/net/poller.hpp"
#include "iris/net/socket.hpp"
#if defined(IRIS_HAVE_IOURING)
    #include "iris/net/iou_send.hpp"
#endif
#if defined(IRIS_HAVE_TLS)
    #include "iris/net/tls.hpp"
#endif
#if defined(IRIS_HA)
    #include "ha_async_db.hpp"
#endif
#include "iris/http/date.hpp"
#include "iris/http/parser.hpp"
#include "iris/http/response.hpp"

#if defined(IRIS_HAVE_LIBPQ)
    #include "iris/db/pg.hpp"
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__linux__)
    #include <sys/sendfile.h>
#endif

#if defined(__linux__)
    #include <linux/filter.h>
    #include <pthread.h>
    #include <sched.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace iris::net {

int make_listener(std::uint16_t port, bool reuseport, int backlog) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#if defined(SO_REUSEPORT)
    if (reuseport) ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#else
    (void)reuseport;
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, backlog) != 0) {
        ::close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

namespace {

// Max bytes any single response can occupy; drain() guarantees this much room
// before invoking the handler. Sized for /queries (up to 500 rows of JSON).
#if defined(IRIS_HA)
constexpr std::size_t kRespReserve = 102400;
#else
constexpr std::size_t kRespReserve = 24576;
#endif

// TFB World table key space and the per-request query fan-out clamp.
constexpr int kWorldRows  = 10000;
constexpr int kMaxQueries = 500;

#if defined(__linux__)
void pin_to_cpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}
#else
void pin_to_cpu(int /*cpu*/) noexcept {}
#endif

// Per-worker PRNG for World ids. xorshift64 is plenty for picking a uniform id
// in [1, kWorldRows]; the bias from % is negligible at this range and TFB does
// not require cryptographic randomness.
std::uint32_t rng_next() noexcept {
    static thread_local std::uint64_t s = [] {
        auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        return static_cast<std::uint64_t>(t) ^
               (reinterpret_cast<std::uint64_t>(&errno) * 0x9E3779B97F4A7C15ull) ^ 0x2545F4914F6CDD1Dull;
    }();
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s);
}
int random_world_id() noexcept {
    return 1 + static_cast<int>(rng_next() % kWorldRows);
}

enum class CState : std::uint8_t {
    kActive,
    kAwaitDb,
    kTlsHandshake,
#if defined(IRIS_HA)
    kUploadDrain,
#endif
};

// Grow-on-demand ring buffer (power-of-two capacity, monotonic indices).
// Replaces std::deque for the DB wait queues: steady state never allocates or
// frees chunks; growth happens only at a new peak queue depth.
template <typename T>
class Ring {
public:
    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(tail_ - head_);
    }
    [[nodiscard]] T& front() noexcept { return buf_[head_ & mask_]; }
    void pop_front() noexcept { ++head_; }
    void push_back(const T& v) {
        if (size() == buf_.size()) grow();
        buf_[tail_ & mask_] = v;
        ++tail_;
    }
    void push_front(const T& v) {
        if (size() == buf_.size()) grow();
        --head_;
        buf_[head_ & mask_] = v;
    }

private:
    void grow() {
        const std::size_t ncap = buf_.empty() ? 64 : buf_.size() * 2;
        std::vector<T>    nbuf(ncap);
        const std::size_t n = size();
        for (std::size_t i = 0; i < n; ++i) nbuf[i] = buf_[(head_ + i) & mask_];
        buf_  = std::move(nbuf);
        mask_ = ncap - 1;
        head_ = 0;
        tail_ = n;
    }
    std::vector<T> buf_;
    std::size_t    mask_ = 0;
    std::uint64_t  head_ = 0;
    std::uint64_t  tail_ = 0;
};

struct Connection {
    int           fd                = -1;
    CState        state             = CState::kActive;
    int           db_slot           = -1;     // owning DB pool slot while serving
    std::uint32_t gen               = 0;      // bumped on release; invalidates
                                              //   stale waiter-queue entries
    DbRoute       pend_route        = DbRoute::kWorldOne;  // queued request intent
    int           pend_count        = 0;                   //   "
#if defined(IRIS_HA)
    int           ha_min            = 10;
    int           ha_max            = 50;
    std::size_t   upload_total      = 0;
    std::size_t   upload_remain     = 0;
#endif
#if defined(IRIS_WFB)
    char          pend_email[256]   = {};
    int           pend_email_len    = 0;
#endif
    int           req_minor         = 1;      // stashed at suspend for the reply
    bool          req_keep_alive    = true;   //   "
    bool          want_write        = false;
    bool          close_after_flush = false;
    bool          read_paused       = false;  // poller read interest dropped
                                              //   (rbuf full while suspended)
    std::size_t   rlen  = 0;   // valid bytes in rbuf
    std::size_t   wlen  = 0;   // valid bytes in wbuf
    std::size_t   wsent = 0;   // bytes of wbuf already written
    std::size_t   rcap  = 0;
    std::size_t   wcap  = 0;
    char*         rbuf  = nullptr;
    char*         wbuf  = nullptr;
    // Tier 1: header in wbuf + body pointer sent via writev (xbody set).
    // Tier 2: full frozen response in a sealed memfd sent via sendfile (xfd set).
    // The two modes are mutually exclusive per response.
    const char*   xbody = nullptr;
    int           xfd   = -1;
    std::size_t   xoff  = 0;
    std::size_t   xlen  = 0;
    std::size_t   xsent = 0;
    bool          uring_inflight = false;
#if defined(IRIS_HAVE_TLS)
    void*         ssl            = nullptr;
#endif
};

#if defined(IRIS_HAVE_LIBPQ)
// Max /db requests from DIFFERENT HTTP connections folded into one pipeline
// on one DB connection. Under load this amortizes the PG socket syscalls and
// epoll wakeups across the whole batch; at low load batches are size 1 and
// behave exactly like a single dispatch (no added latency).
constexpr int kMaxBatch = 32;

// One in-flight DB job. Lives in the worker's DB pool (one per connection
// slot), not per HTTP connection, so the row accumulators cost
// pool_per_worker * ~4KB rather than per-client. kWorldOne jobs serve a BATCH
// of suspended HTTP connections (bconn[0..bn)); every other route serves the
// single connection in `http`.
struct DbJob {
    Connection*  http        = nullptr;  // suspended HTTP conn (null => orphaned)
    DbRoute      route       = DbRoute::kWorldOne;
    int          total       = 0;        // queries requested
    int          recv        = 0;        // tuple results received
    int          sent        = 0;        // queries dispatched
    bool         want_write  = false;    // flush blocked; waiting for writable
    bool         failed      = false;    // a result came back in error
    bool         active      = false;    // a query is in flight on this slot
    bool         pipelined   = false;    // select fan-out used pipeline mode
    bool         sync_seen   = false;    // PGRES_PIPELINE_SYNC consumed
    int          html_len    = -1;       // /fortunes: prebuilt body length in
                                        //   tls_body (-1 => not built)
    int          bn          = 0;        // /db batch: member count
    int          bdone       = 0;        // /db batch: members responded
    Connection*  bconn[kMaxBatch];       // /db batch: members (null => gone)
    std::int32_t ids[kMaxQueries];
    std::int32_t rns[kMaxQueries];
#if defined(IRIS_WFB)
    int          prof_phase       = 0;   // 1=phase1 in flight, 2=phase2
    bool         user_found       = false;
    std::int32_t user_id          = 0;
    int          n_prof_posts     = 0;
    int          n_prof_trending  = 0;
    int          prof_json_len    = -1;
    char         prof_username[256];
    char         prof_email[256];
    char         prof_created_at[64];
    char         prof_last_login[64];
    char         prof_settings[512];
    struct WfbPostRow {
        std::int32_t id;
        std::int32_t views;
        char         title[256];
        char         content[256];
        char         created_at[64];
    };
    WfbPostRow prof_posts[10];
    WfbPostRow prof_trending[5];
#endif
};

// Upper bound on Fortune rows we render (12 canonical + 1 appended + slack).
constexpr int kMaxFortunes = 64;

// Prepared-statement names + SQL, created on every pooled connection at bringup.
constexpr const char* kStmtWorldSelect = "iris_world_select";
constexpr const char* kSqlWorldSelect  =
    "SELECT id, randomNumber FROM World WHERE id = $1";
constexpr const char* kStmtFortuneAll  = "iris_fortune_all";
constexpr const char* kSqlFortuneAll   = "SELECT id, message FROM Fortune";
// /updates bulk write: one prepared statement taking the (id, rn) pairs as two
// parallel int4 arrays. Replaces per-request SQL text assembly: nothing to
// build or parse per request beyond two small array literals.
constexpr const char* kStmtWorldBulk = "iris_world_bulk";
constexpr const char* kSqlWorldBulk  =
    "UPDATE World SET randomNumber = u.rn "
    "FROM unnest($1::int4[], $2::int4[]) AS u(id, rn) WHERE World.id = u.id";

#if defined(IRIS_WFB)
constexpr const char* kStmtWfbUser     = "iris_wfb_user";
constexpr const char* kSqlWfbUser        =
    "SELECT id, username, email, created_at, last_login, settings "
    "FROM users WHERE email = $1";
constexpr const char* kStmtWfbTrending = "iris_wfb_trending";
constexpr const char* kSqlWfbTrending    =
    "SELECT id, title, content, views, created_at FROM posts "
    "ORDER BY views DESC LIMIT 5";
constexpr const char* kStmtWfbUpdate   = "iris_wfb_update";
constexpr const char* kSqlWfbUpdate      =
    "UPDATE users SET last_login = NOW() WHERE id = $1 RETURNING last_login";
constexpr const char* kStmtWfbPosts    = "iris_wfb_posts";
constexpr const char* kSqlWfbPosts       =
    "SELECT id, title, content, views, created_at FROM posts "
    "WHERE user_id = $1 ORDER BY created_at DESC LIMIT 10";
#endif
#if defined(IRIS_HA)
constexpr const char* kStmtHaAsyncDb = "iris_ha_async_db";
constexpr const char* kSqlHaAsyncDb  =
    "SELECT id, name, category, price, quantity, active, tags, rating_score, "
    "rating_count FROM items WHERE price BETWEEN $1 AND $2 LIMIT $3";
#endif
#endif  // IRIS_HAVE_LIBPQ

#if defined(IRIS_HAVE_TLS)
inline ssl_st* conn_ssl(Connection& c) noexcept {
    return static_cast<ssl_st*>(c.ssl);
}
#endif

struct Worker {
    Poller                    poller;
    int                       listener = -1;
#if defined(IRIS_HAVE_TLS)
    int                       tls_listener = -1;
#endif
    Handler                   handler  = nullptr;
    std::size_t               rcap     = 4096;
    std::size_t               wcap     = 32768;
    std::vector<Connection*>  by_fd;
    std::vector<Connection*>  freelist;

#if defined(IRIS_HAVE_LIBPQ)
    bool                      db_enabled = false;
    DbFormatters              fmt{};
    std::vector<db::PgConn>   db_conns;
    std::vector<DbJob>        db_jobs;
    std::vector<int>          db_idle;        // free slot indices
    std::vector<int>          db_slot_by_fd;  // fd -> slot, -1 if not a DB fd
    std::string               db_conninfo;
    // FIFO of connections suspended because every DB slot was busy. Each entry
    // carries the connection's generation so a recycled connection is skipped.
    Ring<std::pair<Connection*, std::uint32_t>> db_waiters;
    // FIFO of suspended /db requests awaiting batch dispatch. kWorldOne always
    // queues here; the dispatcher drains up to kMaxBatch entries per idle slot.
    Ring<std::pair<Connection*, std::uint32_t>> db_one_queue;
#endif

#if defined(IRIS_HAVE_IOURING)
    IouWorker                 iou{};
#endif

    Connection* acquire(int fd) {
        Connection* c;
        if (!freelist.empty()) {
            c = freelist.back();
            freelist.pop_back();
        } else {
            c = new Connection();
            c->rbuf = new char[rcap];
            c->wbuf = new char[wcap];
            c->rcap = rcap;
            c->wcap = wcap;
        }
        c->fd = fd;
        c->state = CState::kActive;
        c->db_slot = -1;
        c->want_write = false;
        c->close_after_flush = false;
        c->read_paused = false;
        c->rlen = c->wlen = c->wsent = 0;
        c->xbody = nullptr;
        c->xfd   = -1;
        c->xoff  = 0;
        c->xlen = c->xsent = 0;
        c->uring_inflight = false;
#if defined(IRIS_HA)
        c->upload_total = c->upload_remain = 0;
#endif
#if defined(IRIS_HAVE_TLS)
        c->ssl = nullptr;
#endif
        if (static_cast<std::size_t>(fd) >= by_fd.size()) by_fd.resize(fd + 1, nullptr);
        by_fd[fd] = c;
        return c;
    }

    void release(Connection* c) {
#if defined(IRIS_HAVE_LIBPQ)
        // If a DB query is still in flight for this connection, orphan its job
        // (or its batch membership) rather than reusing the slot: the result
        // must still be drained before the DB connection can serve again.
        if (c->db_slot >= 0) {
            DbJob& j = db_jobs[c->db_slot];
            if (j.http == c) {
                j.http = nullptr;
            } else {
                for (int i = j.bdone; i < j.bn; ++i) {
                    if (j.bconn[i] == c) { j.bconn[i] = nullptr; break; }
                }
            }
            c->db_slot = -1;
        }
#endif
#if defined(IRIS_HAVE_TLS)
        if (c->ssl != nullptr) {
            iris::net::tls::free_conn(conn_ssl(*c));
            c->ssl = nullptr;
        }
#endif
        if (c->fd >= 0) {
            poller.del(c->fd);
            ::close(c->fd);
            if (static_cast<std::size_t>(c->fd) < by_fd.size()) by_fd[c->fd] = nullptr;
            c->fd = -1;
        }
        // Invalidate any queued waiter entry that still points at this object
        // (it will be reused for a different client).
        ++c->gen;
        c->state = CState::kActive;
        freelist.push_back(c);
    }

    Connection* find(int fd) const {
        return (static_cast<std::size_t>(fd) < by_fd.size()) ? by_fd[fd] : nullptr;
    }
};

// Forward decls (sync path).
bool flush(Worker& w, Connection& c);
bool drain(Worker& w, Connection& c);

#if defined(IRIS_HAVE_TLS)
inline ssize_t conn_recv(Connection& c, char* buf, std::size_t len) noexcept {
    return iris::net::tls::read(conn_ssl(c), c.fd, buf, len);
}

inline ssize_t conn_send(Connection& c, const char* buf, std::size_t len) noexcept {
    return iris::net::tls::write(conn_ssl(c), c.fd, buf, len);
}
#else
inline ssize_t conn_recv(Connection& c, char* buf, std::size_t len) noexcept {
    return ::recv(c.fd, buf, len, 0);
}

inline ssize_t conn_send(Connection& c, const char* buf, std::size_t len) noexcept {
    return ::send(c.fd, buf, len, MSG_NOSIGNAL);
}
#endif

#if defined(IRIS_HAVE_IOURING)
void on_iou_send_done(Worker& w, Connection& c, int res) noexcept;
#endif

// ---- async DB engine --------------------------------------------------------
#if defined(IRIS_HAVE_LIBPQ)

// Response body scratch. Built first (so Content-Length is known), then handed
// to write_response. 32 KiB covers /queries' 500-row array and /fortunes.
#if defined(IRIS_HA)
thread_local char tls_body[98304];
#else
thread_local char tls_body[32768];
#endif

// Forward decls for the mutually-recursive release <-> dispatch path.
void db_start_on_slot(Worker& w, int slot, Connection* c);
void db_fail(Worker& w, int slot);
void db_recover(Worker& w, int slot);
void db_kick_one_queue(Worker& w);
void db_return_slot(Worker& w, int slot, bool force_recover);

int db_acquire(Worker& w) {
    if (w.db_idle.empty()) return -1;
    int slot = w.db_idle.back();
    w.db_idle.pop_back();
    return slot;
}

// Return a slot to the pool, then immediately hand it to the next valid waiter
// (FIFO). Skips stale waiters (client gone / connection recycled).
void db_release(Worker& w, int slot) {
    DbJob& j = w.db_jobs[slot];
    j.http = nullptr;
    j.recv = j.sent = j.total = 0;
    j.failed = false;
    j.active = false;
    j.want_write = false;
    j.pipelined = false;
    j.sync_seen = false;
    j.html_len = -1;
    j.bn = j.bdone = 0;
#if defined(IRIS_WFB)
    j.prof_phase      = 0;
    j.user_found      = false;
    j.user_id         = 0;
    j.n_prof_posts    = 0;
    j.n_prof_trending = 0;
    j.prof_json_len   = -1;
#endif
    w.db_idle.push_back(slot);

    while (!w.db_waiters.empty()) {
        auto [c, gen] = w.db_waiters.front();
        w.db_waiters.pop_front();
        if (c->gen != gen || c->state != CState::kAwaitDb || c->db_slot != -1)
            continue;  // stale entry
        int s = db_acquire(w);
        if (s < 0) {  // someone raced us; requeue and stop
            w.db_waiters.push_front({c, gen});
            break;
        }
        db_start_on_slot(w, s, c);
        break;
    }
    // Any capacity left over goes to pending /db batches.
    db_kick_one_queue(w);
}

// Big-endian encode for binary int4 query parameters: skips the snprintf here
// AND the server-side text->int parse the text format would force.
inline void be32(char* p, std::uint32_t v) noexcept {
    p[0] = static_cast<char>(v >> 24);
    p[1] = static_cast<char>(v >> 16);
    p[2] = static_cast<char>(v >> 8);
    p[3] = static_cast<char>(v);
}
constexpr int kParamLen4[1]  = {4};
constexpr int kParamFmtBin[1] = {1};

// Append a positive integer's decimal digits to `p`; returns chars written.
inline std::size_t put_uint(char* p, std::uint32_t v) noexcept {
    char tmp[10];
    int  n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; ++i) p[i] = tmp[n - 1 - i];
    return static_cast<std::size_t>(n);
}

bool db_send_world_select(Worker& w, int slot) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    char idbuf[4];
    be32(idbuf, static_cast<std::uint32_t>(random_world_id()));
    const char* values[1] = {idbuf};
    if (!pc.send_prepared(kStmtWorldSelect, 1, values, kParamLen4, kParamFmtBin,
                          /*result_binary=*/true)) {
        return false;
    }
    ++j.sent;
    return true;
}

#if defined(IRIS_HA)
bool db_send_ha_async_db(Worker& w, int slot, int min_p, int max_p,
                         int limit) noexcept {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    char        pmin[4], pmax[4], plim[4];
    be32(pmin, static_cast<std::uint32_t>(min_p));
    be32(pmax, static_cast<std::uint32_t>(max_p));
    be32(plim, static_cast<std::uint32_t>(limit));
    const char* values[3] = {pmin, pmax, plim};
    constexpr int kLens[3] = {4, 4, 4};
    constexpr int kFmts[3] = {1, 1, 1};
    if (!pc.send_prepared(kStmtHaAsyncDb, 3, values, kLens, kFmts,
                          /*result_binary=*/false)) {
        return false;
    }
    j.sent = 1;
    return true;
}

void db_respond_ha_empty(Worker& w, Connection* c) noexcept {
    if (c == nullptr) return;
    iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
    iris::http::write_response(ob, 200, "OK", "application/json",
                               "{\"items\":[],\"count\":0}", c->req_minor,
                               c->req_keep_alive);
    c->wlen = ob.size();
    if (!c->req_keep_alive) c->close_after_flush = true;
}
#endif

// Enter pipeline mode and fan out `n` independent World selects (PG14+). TFB
// forbids collapsing the reads into a single IN/ANY query, so each id is its
// own prepared-statement execution; pipelining only batches the round-trip.
// `ids` selects fixed ids (the /updates pre-generated set); nullptr draws a
// fresh random id per select (/queries). Does NOT sync: the caller appends any
// further pipelined commands and then calls pipeline_sync().
bool db_send_select_batch(Worker& w, int slot, int n, const std::int32_t* ids) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    if (!pc.pipeline_enter()) return false;
    j.pipelined = true;
    char idbuf[4];
    for (int i = 0; i < n; ++i) {
        be32(idbuf, static_cast<std::uint32_t>(ids ? ids[i] : random_world_id()));
        const char* values[1] = {idbuf};
        if (!pc.send_prepared(kStmtWorldSelect, 1, values, kParamLen4,
                              kParamFmtBin, /*result_binary=*/true)) {
            return false;
        }
    }
    j.sent = n;
    return true;
}

// Queue the /updates bulk write onto the SAME pipeline as its selects: the ids
// and replacement values are generated client-side before dispatch, so nothing
// in the UPDATE depends on the select results. Pipeline order guarantees the
// server executes every read before the write, and the whole route costs ONE
// network round-trip. Params are the two int4 arrays in text form ("{a,b,c}").
bool db_send_bulk_update(Worker& w, int slot) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    char ida[4096], rna[4096];  // 500 ids of <=5 digits + commas + braces + NUL
    std::size_t la = 0, lr = 0;
    ida[la++] = '{';
    rna[lr++] = '{';
    for (int i = 0; i < j.total; ++i) {
        if (i) { ida[la++] = ','; rna[lr++] = ','; }
        la += put_uint(ida + la, static_cast<std::uint32_t>(j.ids[i]));
        lr += put_uint(rna + lr, static_cast<std::uint32_t>(j.rns[i]));
    }
    ida[la++] = '}'; ida[la] = '\0';
    rna[lr++] = '}'; rna[lr] = '\0';
    const char* values[2] = {ida, rna};
    return pc.send_prepared(kStmtWorldBulk, 2, values, nullptr, nullptr,
                            /*result_binary=*/false);
}

#if defined(IRIS_WFB)
inline void copy_sv(char* dst, std::size_t cap, std::string_view sv) noexcept {
    const std::size_t n = sv.size() < cap - 1 ? sv.size() : cap - 1;
    if (n > 0) std::memcpy(dst, sv.data(), n);
    dst[n] = '\0';
}

inline int parse_int_sv(std::string_view sv) noexcept {
    int v = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return v;
}

inline void copy_post_row(DbJob::WfbPostRow& row, const pg_result* r,
                          int i) noexcept {
    row.id      = parse_int_sv(db::text_field(r, i, 0));
    row.views   = parse_int_sv(db::text_field(r, i, 3));
    copy_sv(row.title, sizeof(row.title), db::text_field(r, i, 1));
    copy_sv(row.content, sizeof(row.content), db::text_field(r, i, 2));
    copy_sv(row.created_at, sizeof(row.created_at), db::text_field(r, i, 4));
}

void json_escape_str(iris::http::Buffer& out, std::string_view s) noexcept {
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:   out.append(c);
        }
    }
}

void wfb_write_post(iris::http::Buffer& out, const DbJob::WfbPostRow& p) noexcept {
    out.append("{\"id\":");
    out.append_uint(static_cast<std::size_t>(p.id));
    out.append(",\"title\":\"");
    json_escape_str(out, p.title);
    out.append("\",\"content\":\"");
    json_escape_str(out, p.content);
    out.append("\",\"views\":");
    out.append_uint(static_cast<std::size_t>(p.views));
    out.append(",\"createdAt\":\"");
    json_escape_str(out, p.created_at);
    out.append("Z\"}");
}

void wfb_build_profile_json(DbJob& j) noexcept {
    iris::http::Buffer out(tls_body, sizeof(tls_body));
    out.append("{\"username\":\"");
    json_escape_str(out, j.prof_username);
    out.append("\",\"email\":\"");
    json_escape_str(out, j.prof_email);
    out.append("\",\"createdAt\":\"");
    json_escape_str(out, j.prof_created_at);
    out.append("Z\",\"lastLogin\":\"");
    json_escape_str(out, j.prof_last_login);
    out.append("Z\",\"settings\":");
    out.append(j.prof_settings);
    out.append(",\"posts\":[");
    for (int i = 0; i < j.n_prof_posts; ++i) {
        if (i) out.append(',');
        wfb_write_post(out, j.prof_posts[i]);
    }
    out.append("],\"trending\":[");
    for (int i = 0; i < j.n_prof_trending; ++i) {
        if (i) out.append(',');
        wfb_write_post(out, j.prof_trending[i]);
    }
    out.append("]}");
    j.prof_json_len = static_cast<int>(out.size());
}

bool db_send_wfb_phase1(Worker& w, int slot, const char* email, int email_len) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    if (!pc.pipeline_enter()) return false;
    j.pipelined = true;
    const char* values[1]   = {email};
    const int   lengths[1]  = {email_len};
    const int   formats[1]  = {0};
    if (!pc.send_prepared(kStmtWfbUser, 1, values, lengths, formats,
                          /*result_binary=*/false)) {
        return false;
    }
    ++j.sent;
    if (!pc.send_prepared(kStmtWfbTrending, 0, nullptr, nullptr, nullptr,
                          /*result_binary=*/false)) {
        return false;
    }
    ++j.sent;
    return pc.pipeline_sync();
}

bool db_send_wfb_phase2(Worker& w, int slot) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    if (!pc.pipeline_enter()) return false;
    j.pipelined = true;
    char idbuf[16];
    const int n = std::snprintf(idbuf, sizeof(idbuf), "%d", j.user_id);
    const char* values[1]  = {idbuf};
    const int   lengths[1] = {n};
    const int   formats[1] = {0};
    if (!pc.send_prepared(kStmtWfbUpdate, 1, values, lengths, formats,
                          /*result_binary=*/false)) {
        return false;
    }
    ++j.sent;
    if (!pc.send_prepared(kStmtWfbPosts, 1, values, lengths, formats,
                          /*result_binary=*/false)) {
        return false;
    }
    ++j.sent;
    return pc.pipeline_sync();
}

void wfb_finish_404(Worker& w, int slot, bool force_recover) {
    DbJob&      j = w.db_jobs[slot];
    Connection* c = j.http;
    if (c == nullptr) {
        db_return_slot(w, slot, force_recover);
        return;
    }
    iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
    iris::http::write_response(ob, 404, "Not Found", "text/plain", "",
                               c->req_minor, c->req_keep_alive);
    c->wlen = ob.size();
    if (!c->req_keep_alive) c->close_after_flush = true;
    c->db_slot = -1;
    c->state   = CState::kActive;
    db_return_slot(w, slot, force_recover);
    if (c->read_paused) {
        w.poller.mod(c->fd, kReadable);
        c->read_paused = false;
    }
    if (!flush(w, *c)) { w.release(c); return; }
    if (c->state == CState::kActive && c->rlen > 0) {
        if (!drain(w, *c)) w.release(c);
    }
}
#endif  // IRIS_WFB

// Sort the (id, rn) accumulators by id.
// the standard guard against deadlocks when concurrent /updates transactions
// touch overlapping rows. n is small (<=500), so an insertion sort on the
// parallel arrays is cache-friendly and avoids packing into pairs.
void db_sort_by_id(std::int32_t* ids, std::int32_t* rns, int n) {
    for (int i = 1; i < n; ++i) {
        const std::int32_t id = ids[i], rn = rns[i];
        int k = i - 1;
        while (k >= 0 && ids[k] > id) {
            ids[k + 1] = ids[k];
            rns[k + 1] = rns[k];
            --k;
        }
        ids[k + 1] = id;
        rns[k + 1] = rn;
    }
}

// Flush buffered output and register/refresh writable interest if the flush is
// incomplete. Returns false on a send error (caller must fail the job).
[[nodiscard]] bool db_arm_flush(Worker& w, int slot) {
    DbJob& j  = w.db_jobs[slot];
    int    fd = w.db_conns[slot].socket();
    const int f = w.db_conns[slot].flush();
    if (f < 0) return false;
    if (f == 1) {
        if (!j.want_write) {
            w.poller.mod(fd, kReadable | kWritable);
            j.want_write = true;
        }
    } else if (j.want_write) {
        w.poller.mod(fd, kReadable);
        j.want_write = false;
    }
    return true;
}

// ---- /db cross-request batching ----------------------------------------------

// Finish ONE /db batch member: build its World-row response, resume the HTTP
// connection, flush, and drain any pipelined bytes. Called as each tuple
// result arrives (results come back in send order).
void db_respond_one(Worker& w, Connection* c, std::int32_t id, std::int32_t rn) {
    c->db_slot = -1;
    c->state   = CState::kActive;

    iris::http::Buffer body(tls_body, sizeof(tls_body));
    if (w.fmt.world_one) w.fmt.world_one(body, id, rn);

    const std::size_t  before = c->wlen;
    iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
    iris::http::write_response(ob, 200, "OK", "application/json", body.view(),
                               c->req_minor, c->req_keep_alive);
    if (ob.overflow()) {
        c->wlen = before;
        iris::http::Buffer eb(c->wbuf, c->wcap, c->wlen);
        iris::http::write_response(eb, 500, "Internal Server Error", "text/plain",
                                   "response overflow", c->req_minor, false);
        c->wlen = eb.size();
        c->close_after_flush = true;
    } else {
        c->wlen = ob.size();
        if (!c->req_keep_alive) c->close_after_flush = true;
    }
    if (c->read_paused) {
        w.poller.mod(c->fd, kReadable);
        c->read_paused = false;
    }
    if (!flush(w, *c)) { w.release(c); return; }
    if (c->state == CState::kActive && c->rlen > 0) {
        if (!drain(w, *c)) w.release(c);
    }
}

// Fail ONE /db batch member with a 500 (its select errored or the batch's
// connection died).
void db_error_one(Worker& w, Connection* c) {
    c->db_slot = -1;
    c->state   = CState::kActive;
    iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
    iris::http::write_response(ob, 500, "Internal Server Error", "text/plain",
                               "db error", c->req_minor, false);
    c->wlen = ob.size();
    c->close_after_flush = true;
    if (c->read_paused) {
        w.poller.mod(c->fd, kReadable);
        c->read_paused = false;
    }
    if (!flush(w, *c)) w.release(c);
}

// Pipeline `k` already-collected /db members (j.bconn[0..k)) onto `slot`: one
// random World select per member, one sync, one flush for the whole batch.
void db_start_one_batch(Worker& w, int slot, int k) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];
    j.http       = nullptr;
    j.route      = DbRoute::kWorldOne;
    j.total      = k;
    j.recv       = 0;
    j.sent       = 0;
    j.failed     = false;
    j.active     = true;
    j.want_write = false;
    j.pipelined  = false;
    j.sync_seen  = false;
    j.html_len   = -1;
    j.bn         = k;
    j.bdone      = 0;
    for (int i = 0; i < k; ++i) j.bconn[i]->db_slot = slot;

    bool ok = pc.pipeline_enter();
    if (ok) {
        j.pipelined = true;
        char idbuf[4];
        for (int i = 0; ok && i < k; ++i) {
            be32(idbuf, static_cast<std::uint32_t>(random_world_id()));
            const char* values[1] = {idbuf};
            ok = pc.send_prepared(kStmtWorldSelect, 1, values, kParamLen4,
                                  kParamFmtBin, /*result_binary=*/true);
        }
        if (ok) ok = pc.pipeline_sync();
        j.sent = k;
    }
    if (!ok || !db_arm_flush(w, slot)) db_fail(w, slot);
}

// Drain the /db queue onto idle slots, batching up to kMaxBatch members per
// slot. Skips stale entries (client gone / connection recycled).
void db_kick_one_queue(Worker& w) {
    while (!w.db_one_queue.empty()) {
        int slot = db_acquire(w);
        if (slot < 0) return;
        DbJob& j = w.db_jobs[slot];
        int    k = 0;
        while (k < kMaxBatch && !w.db_one_queue.empty()) {
            auto [c, gen] = w.db_one_queue.front();
            w.db_one_queue.pop_front();
            if (c->gen != gen || c->state != CState::kAwaitDb || c->db_slot != -1)
                continue;  // stale entry
            j.bconn[k++] = c;
        }
        if (k == 0) {  // queue held only stale entries
            w.db_idle.push_back(slot);
            return;
        }
        db_start_one_batch(w, slot, k);
    }
}

// Bind connection `c`'s pending request to DB pool `slot`, dispatch the query,
// and mark the connection as serving on that slot. On dispatch failure the
// request is failed (500) and the slot recovered.
void db_start_on_slot(Worker& w, int slot, Connection* c) {
    DbJob& j     = w.db_jobs[slot];
    j.http       = c;
    j.route      = c->pend_route;
    j.recv       = 0;
    j.sent       = 0;
    j.failed     = false;
    j.active     = true;
    j.want_write = false;
    j.pipelined  = false;
    j.sync_seen  = false;
    j.html_len   = -1;

    const int n = c->pend_count < 1 ? 1
                : (c->pend_count > kMaxQueries ? kMaxQueries : c->pend_count);

    bool dispatched = false;
    switch (c->pend_route) {
        case DbRoute::kWorldOne:
            j.total    = 1;
            dispatched = db_send_world_select(w, slot);
            break;
        case DbRoute::kWorldMany:
            j.total    = n;
            dispatched = db_send_select_batch(w, slot, n, nullptr) &&
                         w.db_conns[slot].pipeline_sync();
            break;
        case DbRoute::kWorldUpdate:
            // Generate ids and replacement values up front, sorted by id (the
            // ascending lock order is the deadlock guard), then pipeline the N
            // mandated row reads AND the bulk write in one round-trip.
            j.total = n;
            for (int i = 0; i < n; ++i) {
                j.ids[i] = random_world_id();
                j.rns[i] = random_world_id();
            }
            db_sort_by_id(j.ids, j.rns, n);
            dispatched = db_send_select_batch(w, slot, n, j.ids) &&
                         db_send_bulk_update(w, slot) &&
                         w.db_conns[slot].pipeline_sync();
            break;
        case DbRoute::kFortunes:
            // Single prepared query, binary result, NOT pipelined: same shape as
            // db_send_world_select. The extended protocol skips the per-request
            // server-side parse/plan that the old simple-query path paid on every
            // hit, and binary int4 ids avoid a text decode.
            j.total    = 0;
            dispatched = w.db_conns[slot].send_prepared(
                kStmtFortuneAll, 0, nullptr, nullptr, nullptr,
                /*result_binary=*/true);
            ++j.sent;
            break;
#if defined(IRIS_HA)
        case DbRoute::kHaAsyncDb:
            j.total    = c->pend_count;
            dispatched = db_send_ha_async_db(w, slot, c->ha_min, c->ha_max,
                                             c->pend_count);
            break;
#endif
#if defined(IRIS_WFB)
        case DbRoute::kUserProfile:
            copy_sv(j.prof_email, sizeof(j.prof_email),
                    std::string_view(c->pend_email,
                                     static_cast<std::size_t>(c->pend_email_len)));
            j.prof_phase      = 1;
            j.user_found      = false;
            j.n_prof_posts    = 0;
            j.n_prof_trending = 0;
            j.prof_json_len   = -1;
            j.total           = 2;
            dispatched        = db_send_wfb_phase1(w, slot, c->pend_email,
                                                     c->pend_email_len);
            break;
#endif
    }

    if (!dispatched) {
        c->db_slot = -1;
        db_fail(w, slot);
        return;
    }
    c->db_slot = slot;
    if (!db_arm_flush(w, slot)) db_fail(w, slot);
}

// Return a finished slot to the pool, but reconnect first if the connection is
// no longer in a clean idle state (bad link, or stuck in pipeline mode). This
// is what keeps a connection reusable across different route types.
void db_return_slot(Worker& w, int slot, bool force_recover) {
    if (force_recover || w.db_conns[slot].is_bad()) {
        db_recover(w, slot);
    } else {
        db_release(w, slot);
    }
}

// Finish a completed job: format the body, assemble the response onto the HTTP
// connection, return the DB slot, then resume the HTTP connection.
void db_finish_ex(Worker& w, int slot, bool force_recover) {
    DbJob&      j = w.db_jobs[slot];
    Connection* c = j.http;

    if (c == nullptr) {        // orphaned (client vanished mid-query)
        db_return_slot(w, slot, force_recover);
        return;
    }

    iris::http::Buffer body(tls_body, sizeof(tls_body));
    std::string_view   ctype = "application/json";

    if (j.failed) {
        // DB-level error: 500 and close. Rare; keeps the pool honest.
        iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
        iris::http::write_response(ob, 500, "Internal Server Error", "text/plain",
                                   "db error", c->req_minor, false);
        c->wlen = ob.size();
        c->close_after_flush = true;
    } else {
        std::string_view payload;
        switch (j.route) {
            case DbRoute::kWorldOne:
                if (w.fmt.world_one) w.fmt.world_one(body, j.ids[0], j.rns[0]);
                payload = body.view();
                break;
            case DbRoute::kWorldMany:
            case DbRoute::kWorldUpdate:
                if (w.fmt.world_many)
                    w.fmt.world_many(body, j.ids, j.rns, j.recv);
                payload = body.view();
                break;
            case DbRoute::kFortunes:
                ctype = "text/html; charset=UTF-8";
                // Body was rendered into tls_body during db_pump (messages were
                // still live then); emit it verbatim.
                payload = std::string_view(tls_body,
                              j.html_len > 0 ? static_cast<std::size_t>(j.html_len) : 0);
                break;
#if defined(IRIS_WFB)
            case DbRoute::kUserProfile:
                payload = std::string_view(
                    tls_body,
                    j.prof_json_len > 0 ? static_cast<std::size_t>(j.prof_json_len) : 0);
                break;
#endif
#if defined(IRIS_HA)
            case DbRoute::kHaAsyncDb:
                payload = std::string_view(
                    tls_body,
                    j.html_len > 0 ? static_cast<std::size_t>(j.html_len) : 0);
                break;
#endif
        }
        const std::size_t  before = c->wlen;
        iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
        iris::http::write_response(ob, 200, "OK", ctype, payload, c->req_minor,
                                   c->req_keep_alive);
        if (ob.overflow()) {
            // drain() reserves kRespReserve before suspending, so this is
            // unreachable by construction -- but a truncated 200 would hang the
            // client on a short body, so fail loudly instead.
            c->wlen = before;
            iris::http::Buffer eb(c->wbuf, c->wcap, c->wlen);
            iris::http::write_response(eb, 500, "Internal Server Error",
                                       "text/plain", "response overflow",
                                       c->req_minor, false);
            c->wlen = eb.size();
            c->close_after_flush = true;
        } else {
            c->wlen = ob.size();
            if (!c->req_keep_alive) c->close_after_flush = true;
        }
    }

    // Return the DB slot before touching the HTTP socket so it is available
    // again even if the client write fails.
    c->db_slot = -1;
    c->state   = CState::kActive;
    db_return_slot(w, slot, force_recover);

    if (c->read_paused) {  // rbuf filled while suspended; restore read interest
        w.poller.mod(c->fd, kReadable);
        c->read_paused = false;
    }
    if (!flush(w, *c)) { w.release(c); return; }
    // Drain any pipelined bytes that arrived while suspended.
    if (c->state == CState::kActive && c->rlen > 0) {
        if (!drain(w, *c)) w.release(c);
    }
}

inline void db_finish(Worker& w, int slot) { db_finish_ex(w, slot, false); }

// Drop a slot index out of the idle free list (used when an idle connection
// dies and cannot be re-established). O(pool size), which is tiny.
void db_drop_idle(Worker& w, int slot) {
    for (auto it = w.db_idle.begin(); it != w.db_idle.end(); ++it) {
        if (*it == slot) { w.db_idle.erase(it); return; }
    }
}

// Blocking reconnect + re-prepare of a single slot, re-registering its (new) fd
// in the poller. Returns false if the connection could not be re-established.
bool db_reconnect_slot(Worker& w, int slot) {
    db::PgConn& pc     = w.db_conns[slot];
    const int   old_fd = pc.socket();
    if (old_fd >= 0) {
        w.poller.del(old_fd);
        if (static_cast<std::size_t>(old_fd) < w.db_slot_by_fd.size())
            w.db_slot_by_fd[old_fd] = -1;
    }
    if (!pc.connect(w.db_conninfo.c_str()) ||
#if defined(IRIS_HA)
        !pc.prepare(kStmtHaAsyncDb, kSqlHaAsyncDb, 3)
#else
        !pc.prepare(kStmtWorldSelect, kSqlWorldSelect, 1) ||
        !pc.prepare(kStmtFortuneAll, kSqlFortuneAll, 0) ||
        !pc.prepare(kStmtWorldBulk, kSqlWorldBulk, 2)
#if defined(IRIS_WFB)
        || !pc.prepare(kStmtWfbUser, kSqlWfbUser, 1) ||
        !pc.prepare(kStmtWfbTrending, kSqlWfbTrending, 0) ||
        !pc.prepare(kStmtWfbUpdate, kSqlWfbUpdate, 1) ||
        !pc.prepare(kStmtWfbPosts, kSqlWfbPosts, 1)
#endif
#endif
        ) {
        std::fprintf(stderr, "[iris-gw] DB slot %d reconnect failed: %s\n", slot,
                     pc.error().c_str());
        return false;
    }
    const int fd = pc.socket();
    if (static_cast<std::size_t>(fd) >= w.db_slot_by_fd.size())
        w.db_slot_by_fd.resize(fd + 1, -1);
    w.db_slot_by_fd[fd] = slot;
    w.poller.add(fd, kReadable);
    return true;
}

// Recover a slot whose in-flight query failed: reconnect and return it to the
// pool (db_release), or drop it from rotation if reconnection fails.
void db_recover(Worker& w, int slot) {
    if (db_reconnect_slot(w, slot)) {
        db_release(w, slot);  // back to idle (also pulls the next waiter)
    }
    // else: not in db_idle (it was active), so capacity simply shrinks.
}

// A DB connection-level failure while serving `slot`: fail the HTTP
// request(s), then recover the connection.
void db_fail(Worker& w, int slot) {
    DbJob& j = w.db_jobs[slot];
#if defined(IRIS_HA)
    if (j.route == DbRoute::kHaAsyncDb && j.http != nullptr) {
        Connection* c = j.http;
        db_respond_ha_empty(w, c);
        c->db_slot = -1;
        c->state   = CState::kActive;
        j.http     = nullptr;
        if (c->read_paused) {
            w.poller.mod(c->fd, kReadable);
            c->read_paused = false;
        }
        if (!flush(w, *c)) w.release(c);
        db_recover(w, slot);
        return;
    }
#endif
    // /db batch members that have not been responded to yet.
    for (int i = j.bdone; i < j.bn; ++i) {
        if (j.bconn[i]) db_error_one(w, j.bconn[i]);
        j.bconn[i] = nullptr;
    }
    j.bn = j.bdone = 0;
    if (j.http) {
        Connection* c = j.http;
        iris::http::Buffer ob(c->wbuf, c->wcap, c->wlen);
        iris::http::write_response(ob, 500, "Internal Server Error", "text/plain",
                                   "db error", c->req_minor, false);
        c->wlen = ob.size();
        c->close_after_flush = true;
        c->db_slot = -1;
        c->state = CState::kActive;
        j.http = nullptr;
        if (c->read_paused) {
            w.poller.mod(c->fd, kReadable);
            c->read_paused = false;
        }
        if (!flush(w, *c)) w.release(c);
    }
    db_recover(w, slot);
}

// All results for the current command/pipeline have been consumed (take_result
// returned null past any sync). The job is complete for every route: /updates'
// bulk write rides the same pipeline as its selects, so there is no second
// phase.
void db_on_command_complete(Worker& w, int slot) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];

    // A pipelined fan-out (/db batches, /queries, /updates) leaves the
    // connection in pipeline mode. It MUST exit before the slot is reused by a
    // non-pipelined route (/db single select and /fortunes both send a lone
    // extended-protocol query outside any pipeline). If exit fails, force a
    // reconnect so the pool never hands out a wedged connection.
    bool force_recover = false;
    if (j.pipelined) {
        if (!pc.pipeline_exit()) force_recover = true;
        j.pipelined = false;
    }

    if (j.route == DbRoute::kWorldOne) {
        // /db batch: successes were responded to inline as their tuples
        // arrived; anything left over hit an error result.
        for (int i = j.bdone; i < j.bn; ++i) {
            if (j.bconn[i]) db_error_one(w, j.bconn[i]);
            j.bconn[i] = nullptr;
        }
        j.bn = j.bdone = 0;
        db_return_slot(w, slot, force_recover);
        return;
    }
#if defined(IRIS_WFB)
    if (j.route == DbRoute::kUserProfile && j.prof_phase == 1) {
        if (!j.user_found) {
            wfb_finish_404(w, slot, force_recover);
            return;
        }
        j.prof_phase = 2;
        j.recv = j.sent = 0;
        j.sync_seen = false;
        if (!db_send_wfb_phase2(w, slot)) {
            db_fail(w, slot);
            return;
        }
        if (!db_arm_flush(w, slot)) db_fail(w, slot);
        return;
    }
    if (j.route == DbRoute::kUserProfile && j.prof_phase == 2) {
        wfb_build_profile_json(j);
        db_finish_ex(w, slot, force_recover);
        return;
    }
#endif
    db_finish_ex(w, slot, force_recover);
}

// Drain every result currently buffered on a ready DB connection, threading the
// pipeline's NULL separators and the PGRES_PIPELINE_SYNC marker.
void db_pump(Worker& w, int slot) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];

    if (!pc.consume_input()) { db_fail(w, slot); return; }

    for (;;) {
        if (pc.is_busy()) return;  // results not fully arrived; wait for readable
        pg_result* r = pc.take_result();
        if (r == nullptr) {
            // NULL separates queries in a pipeline; only the NULL past the sync
            // (or the lone NULL of a non-pipelined command) means "complete".
            if (j.pipelined && !j.sync_seen) continue;
            db_on_command_complete(w, slot);
            return;
        }
        if (db::result_is_pipeline_sync(r)) {
            j.sync_seen = true;
        } else if (db::result_is_error(r)) {
            j.failed = true;
#if defined(IRIS_WFB)
        } else if (j.route == DbRoute::kUserProfile && db::result_ok_tuples(r)) {
            if (j.prof_phase == 1) {
                if (j.recv == 0) {
                    if (db::result_rows(r) >= 1) {
                        j.user_found = true;
                        j.user_id    = parse_int_sv(db::text_field(r, 0, 0));
                        copy_sv(j.prof_username, sizeof(j.prof_username),
                                db::text_field(r, 0, 1));
                        copy_sv(j.prof_email, sizeof(j.prof_email),
                                db::text_field(r, 0, 2));
                        copy_sv(j.prof_created_at, sizeof(j.prof_created_at),
                                db::text_field(r, 0, 3));
                        if (db::text_field(r, 0, 4).size() > 0) {
                            copy_sv(j.prof_last_login, sizeof(j.prof_last_login),
                                    db::text_field(r, 0, 4));
                        } else {
                            j.prof_last_login[0] = '\0';
                        }
                        copy_sv(j.prof_settings, sizeof(j.prof_settings),
                                db::text_field(r, 0, 5));
                    } else {
                        j.user_found = false;
                    }
                } else if (j.recv == 1) {
                    j.n_prof_trending = 0;
                    const int rows = db::result_rows(r);
                    for (int i = 0; i < rows && j.n_prof_trending < 5;
                         ++i, ++j.n_prof_trending) {
                        copy_post_row(j.prof_trending[j.n_prof_trending], r, i);
                    }
                }
                ++j.recv;
            } else if (j.prof_phase == 2) {
                if (j.recv == 0 && db::result_rows(r) >= 1) {
                    copy_sv(j.prof_last_login, sizeof(j.prof_last_login),
                            db::text_field(r, 0, 0));
                } else if (j.recv == 1) {
                    j.n_prof_posts = 0;
                    const int rows = db::result_rows(r);
                    for (int i = 0; i < rows && j.n_prof_posts < 10;
                         ++i, ++j.n_prof_posts) {
                        copy_post_row(j.prof_posts[j.n_prof_posts], r, i);
                    }
                }
                ++j.recv;
            }
#endif
        } else if (j.route == DbRoute::kWorldOne && db::result_ok_tuples(r) &&
                   db::result_rows(r) >= 1) {
            // /db batch: results return in send order, so the next tuple
            // belongs to the next unanswered member. Respond immediately --
            // the row data lives in `r`, no accumulation needed.
            const std::int32_t id = db::bin_int4(r, 0, 0);
            const std::int32_t rn = db::bin_int4(r, 0, 1);
            if (j.bdone < j.bn) {
                Connection* c       = j.bconn[j.bdone];
                j.bconn[j.bdone]    = nullptr;
                ++j.bdone;
                if (c) db_respond_one(w, c, id, rn);
            }
            ++j.recv;
#if defined(IRIS_HA)
        } else if (j.route == DbRoute::kHaAsyncDb && db::result_ok_tuples(r)) {
            j.html_len = iris::ha::format_async_db_json(tls_body, sizeof(tls_body), r);
            if (j.html_len < 0) j.failed = true;
            ++j.recv;
#endif
        } else if (j.route == DbRoute::kFortunes && db::result_ok_tuples(r)) {
            // The whole table arrives in one result. Decode + render the HTML
            // body NOW, while the message string_views still point into `r`.
            // Binary result: id is binary int4 (bin_int4); message is the raw
            // varchar bytes (UTF-8), valid via text_field's PQgetlength.
            FortuneRow tmp[kMaxFortunes];
            const int  rows = db::result_rows(r);
            int        m    = 0;
            for (int i = 0; i < rows && m < kMaxFortunes; ++i, ++m) {
                tmp[m].id      = db::bin_int4(r, i, 0);
                tmp[m].message = db::text_field(r, i, 1);
            }
            iris::http::Buffer body(tls_body, sizeof(tls_body));
            if (w.fmt.fortunes) w.fmt.fortunes(body, tmp, m);
            j.html_len = static_cast<int>(body.size());
        } else if (db::result_ok_tuples(r) && db::result_rows(r) >= 1) {
            // /updates already holds its (sorted) ids and replacement values;
            // the select results only confirm the reads happened. Recording
            // them would clobber the new values, so only count those.
            if (j.route != DbRoute::kWorldUpdate) {
                const int idx = j.recv < kMaxQueries ? j.recv : kMaxQueries - 1;
                j.ids[idx] = db::bin_int4(r, 0, 0);
                j.rns[idx] = db::bin_int4(r, 0, 1);
            }
            ++j.recv;
        }
        // The bulk-update COMMAND_OK result carries no payload; fall through.
        db::clear_result(r);
    }
}

void on_db_event(Worker& w, int slot, std::uint32_t flags) {
    DbJob&      j  = w.db_jobs[slot];
    db::PgConn& pc = w.db_conns[slot];

    // An event on an idle slot is not a query result -- it is almost always the
    // server closing an idle connection (EOF). Never run the result pump here
    // (that would db_release an already-idle slot and duplicate it in the free
    // list). Just clear readability and reconnect in place if the link broke.
    if (!j.active) {
        if (flags & kReadable) {
            if (!pc.consume_input() || pc.is_bad()) {
                if (!db_reconnect_slot(w, slot)) db_drop_idle(w, slot);
            }
        }
        return;
    }

    if ((flags & kWritable) && j.want_write) {
        const int f = pc.flush();
        if (f < 0) { db_fail(w, slot); return; }
        if (f == 0) {
            w.poller.mod(pc.socket(), kReadable);
            j.want_write = false;
        }
    }
    if (flags & kReadable) {
        db_pump(w, slot);
    }
}
#endif  // IRIS_HAVE_LIBPQ

// ---- sync HTTP path ---------------------------------------------------------

#if defined(IRIS_HAVE_IOURING)
void on_iou_send_done(Worker& w, Connection& c, int res) noexcept {
    c.uring_inflight = false;
    if (res < 0) {
        w.release(&c);
        return;
    }
    c.xsent += static_cast<std::size_t>(res);
    if (c.xsent < c.xlen) {
        if (!flush(w, c)) w.release(&c);
        return;
    }
    c.wlen  = 0;
    c.wsent = 0;
    c.xbody = nullptr;
    c.xfd   = -1;
    c.xoff  = 0;
    c.xlen  = c.xsent = 0;
    if (c.want_write) {
        w.poller.mod(c.fd, kReadable);
        c.want_write = false;
    }
    if (c.close_after_flush) {
        w.release(&c);
        return;
    }
    if (c.state == CState::kActive && c.rlen > 0) {
        if (!drain(w, c)) w.release(&c);
    }
}
#endif

// Flush pending [wsent, wlen). Returns false to close the connection.
bool flush(Worker& w, Connection& c) {
    // Three emit modes:
    //   1) wbuf only (plain send)
    //   2) wbuf header + xbody tail (writev / send)
    //   3) sealed memfd slice via sendfile (Tier 2 frozen static responses)
    while (c.wsent < c.wlen || (c.xbody && c.xsent < c.xlen) ||
           (c.xfd >= 0 && c.xsent < c.xlen)) {
        ssize_t n;
        const std::size_t hdr_left = c.wlen - c.wsent;
#if defined(__linux__)
        if (hdr_left == 0 && c.xfd >= 0 && c.xsent < c.xlen) {
#if defined(IRIS_HAVE_IOURING)
            if (iou_send_enabled()) {
                if (c.uring_inflight) return true;
                if (iou_submit_send(&w.iou, c.fd, c.xoff + c.xsent,
                                    c.xlen - c.xsent, &c)) {
                    c.uring_inflight = true;
                    iou_reap(&w.iou, &w,
                             [](void* ctx, void* ud, int res) noexcept {
                                 Worker&     wr = *static_cast<Worker*>(ctx);
                                 Connection& cn = *static_cast<Connection*>(ud);
                                 on_iou_send_done(wr, cn, res);
                             });
                    if (!c.uring_inflight) {
                        return !c.close_after_flush;
                    }
                    return true;
                }
            }
#endif
            const std::size_t left = c.xlen - c.xsent;
            off_t             off  = static_cast<off_t>(c.xoff + c.xsent);
            n = ::sendfile(c.fd, c.xfd, &off, left);
        } else
#endif
        if (hdr_left > 0 && c.xbody && c.xsent < c.xlen) {
            struct iovec iov[2];
            iov[0].iov_base = c.wbuf + c.wsent;
            iov[0].iov_len  = hdr_left;
            iov[1].iov_base = const_cast<char*>(c.xbody + c.xsent);
            iov[1].iov_len  = c.xlen - c.xsent;
            n = ::writev(c.fd, iov, 2);
        } else if (hdr_left > 0) {
            n = conn_send(c, c.wbuf + c.wsent, hdr_left);
        } else {
            n = conn_send(c, c.xbody + c.xsent, c.xlen - c.xsent);
        }
        if (n > 0) {
            std::size_t adv = static_cast<std::size_t>(n);
            const std::size_t h = c.wlen - c.wsent;
            if (adv >= h) { c.wsent = c.wlen; adv -= h; c.xsent += adv; }
            else          { c.wsent += adv; }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!c.want_write) {
                w.poller.mod(c.fd, kReadable | kWritable);
                c.want_write = true;
            }
            return true;  // resume on writable
        }
        if (n < 0 && errno == EINTR) continue;
        return false;  // peer reset / fatal
    }
    c.wlen  = 0;
    c.wsent = 0;
    c.xbody = nullptr;
    c.xfd   = -1;
    c.xoff  = 0;
    c.xlen  = c.xsent = 0;
    c.uring_inflight = false;
    if (c.want_write) {
        w.poller.mod(c.fd, kReadable);
        c.want_write = false;
    }
    return !c.close_after_flush;
}

#if defined(IRIS_HA)
inline bool is_ha_upload(const iris::http::Request& req) noexcept {
    if (req.method != "POST") return false;
    const std::string_view path = req.path;
    return path == "/upload" ||
           (path.size() > 7 && path.substr(0, 7) == "/upload" && path[7] == '?');
}

bool upload_respond(Worker& w, Connection& c, std::size_t nbytes) noexcept {
    if (c.wcap - c.wlen < kRespReserve) {
        if (!flush(w, c)) return false;
        if (c.want_write) return true;
    }
    char tmp[24];
    const int n = std::snprintf(tmp, sizeof(tmp), "%zu", nbytes);
    iris::http::Buffer ob(c.wbuf, c.wcap, c.wlen);
    iris::http::write_response(
        ob, 200, "OK", "text/plain",
        std::string_view(tmp, static_cast<std::size_t>(n)), c.req_minor,
        c.req_keep_alive);
    c.wlen = ob.size();
    if (!c.req_keep_alive) c.close_after_flush = true;
    if (!flush(w, c)) return false;
    return true;
}

// Discard buffered upload bytes; respond when upload_remain hits zero.
bool upload_progress(Worker& w, Connection& c) noexcept {
    while (c.upload_remain > 0 && c.rlen > 0) {
        const std::size_t take = std::min(c.rlen, c.upload_remain);
        c.upload_remain -= take;
        if (take < c.rlen) {
            std::memmove(c.rbuf, c.rbuf + take, c.rlen - take);
        }
        c.rlen -= take;
    }
    if (c.upload_remain > 0) return true;

    const std::size_t total = c.upload_total;
    c.upload_total  = 0;
    c.upload_remain = 0;
    c.state         = CState::kActive;
    if (!upload_respond(w, c, total)) return false;
    if (c.want_write || c.close_after_flush) return true;
    return drain(w, c);
}

bool upload_begin(Worker& w, Connection& c, std::size_t off,
                  const iris::http::Request& req, std::size_t header_end) noexcept {
    const std::size_t avail = c.rlen - off;
    const std::size_t body_in_buf =
        avail > header_end ? avail - header_end : 0;

    c.req_minor      = req.minor_version;
    c.req_keep_alive = req.keep_alive;
    c.upload_total   = req.content_length;

    if (body_in_buf >= req.content_length) {
        if (!upload_respond(w, c, req.content_length)) return false;
        const std::size_t consumed = header_end + req.content_length;
        if (consumed < c.rlen) {
            std::memmove(c.rbuf, c.rbuf + consumed, c.rlen - consumed);
        }
        c.rlen -= consumed;
        if (c.want_write || c.close_after_flush) return true;
        return drain(w, c);
    }

    c.upload_remain = req.content_length - body_in_buf;
    c.state         = CState::kUploadDrain;
    const std::size_t consumed = header_end + body_in_buf;
    if (consumed > 0) {
        if (consumed < c.rlen) {
            std::memmove(c.rbuf, c.rbuf + consumed, c.rlen - consumed);
        }
        c.rlen -= consumed;
    }
    return upload_progress(w, c);
}
#endif

// Parse and respond to every complete (pipelined) request currently buffered.
// Stops early (returns true, leaving bytes in rbuf) if the connection suspends
// on an async DB op.
bool drain(Worker& w, Connection& c) {
    std::size_t off = 0;
    while (off < c.rlen) {
        iris::http::Request req;
        auto pr = iris::http::parse_request(c.rbuf + off, c.rlen - off, req);
        if (pr.status == iris::http::ParseStatus::kIncomplete) {
#if defined(IRIS_HA)
            std::size_t header_end = 0;
            auto hr = iris::http::parse_request_headers(c.rbuf + off, c.rlen - off,
                                                        req, header_end);
            if (hr.status == iris::http::ParseStatus::kIncomplete && header_end > 0 &&
                is_ha_upload(req) && req.content_length > 0) {
                if (off > 0) {
                    std::memmove(c.rbuf, c.rbuf + off, c.rlen - off);
                    c.rlen -= off;
                    off = 0;
                }
                return upload_begin(w, c, off, req, header_end);
            }
#endif
            break;
        }
        if (pr.status == iris::http::ParseStatus::kError) return false;

        // Guarantee room for this response before touching the handler.
        if (c.wcap - c.wlen < kRespReserve) {
            if (!flush(w, c)) return false;
            if (c.want_write) {
                std::memmove(c.rbuf, c.rbuf + off, c.rlen - off);
                c.rlen -= off;
                return true;
            }
        }

        AsyncCtx ctx;
        ctx.worker_ = &w;
        ctx.conn_   = &c;
        iris::http::Buffer ob(c.wbuf, c.wcap, c.wlen);
        Outcome out = w.handler(req, ob, ctx);

        if (out == Outcome::kSuspended) {
            // The handler started an async DB op. Consume this request's bytes,
            // stash reply context, and stop draining; the DB completion path
            // resumes us. Any trailing pipelined bytes stay buffered.
            off += pr.consumed;
            c.req_minor      = req.minor_version;
            c.req_keep_alive = req.keep_alive;
            if (off > 0) {
                std::memmove(c.rbuf, c.rbuf + off, c.rlen - off);
                c.rlen -= off;
            }
            return true;
        }

        c.wlen = ob.size();
        off += pr.consumed;

        // Zero-copy tail registered: the body lives outside wbuf and there is
        // exactly one tail slot per connection, so it must be fully sent before
        // the next pipelined response's header is appended. Flush now; if the
        // socket blocks, stash the remaining pipelined bytes and resume later.
        if (c.xbody != nullptr || c.xfd >= 0) {
            if (!req.keep_alive) c.close_after_flush = true;
            if (!flush(w, c)) return false;
            if (c.want_write) {
                if (off > 0) {
                    std::memmove(c.rbuf, c.rbuf + off, c.rlen - off);
                    c.rlen -= off;
                }
                return true;
            }
            if (c.close_after_flush) break;
            continue;
        }

        if (!req.keep_alive) {
            c.close_after_flush = true;
            break;
        }
    }

    if (off > 0) {
        std::memmove(c.rbuf, c.rbuf + off, c.rlen - off);
        c.rlen -= off;
    }
    if (c.wlen > c.wsent) {
        if (!flush(w, c)) return false;
    }
    return true;
}

bool on_readable(Worker& w, Connection& c) {
#if defined(IRIS_HAVE_TLS)
    if (c.state == CState::kTlsHandshake) {
        const auto hs = iris::net::tls::handshake(conn_ssl(c));
        if (hs == iris::net::tls::Handshake::kDone) {
            c.state = CState::kActive;
        } else if (hs == iris::net::tls::Handshake::kWantWrite) {
            if (!c.want_write) {
                w.poller.mod(c.fd, kReadable | kWritable);
                c.want_write = true;
            }
            return true;
        } else if (hs == iris::net::tls::Handshake::kWantRead) {
            return true;
        } else {
            return false;
        }
    }
#endif
    for (;;) {
        if (c.rlen == c.rcap) break;  // buffer full: drain to free space
        ssize_t n = conn_recv(c, c.rbuf + c.rlen, c.rcap - c.rlen);
        if (n > 0) {
            c.rlen += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) return false;  // peer closed
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
    }
    // While suspended on a DB op we accept bytes (to detect close) but must not
    // re-enter the handler; the DB completion path drains them on resume. If
    // the read buffer fills while suspended, drop read interest -- otherwise
    // level-triggered polling would spin on this fd for the whole DB
    // round-trip. Resume re-arms it. (Skipped when a write is armed so the
    // writable notification is not lost; that combination self-resolves.)
    if (c.state == CState::kAwaitDb) {
        if (c.rlen == c.rcap && !c.read_paused && !c.want_write) {
            w.poller.mod(c.fd, 0);
            c.read_paused = true;
        }
        return true;
    }
#if defined(IRIS_HA)
    if (c.state == CState::kUploadDrain) {
        return upload_progress(w, c);
    }
#endif
    return drain(w, c);
}

void on_accept(Worker& w, int listener_fd) {
    const bool is_tls =
#if defined(IRIS_HAVE_TLS)
        (listener_fd == w.tls_listener);
#else
        false;
#endif
    for (;;) {
        int cfd = ::accept(listener_fd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;  // transient (e.g. EMFILE): stop this round, retry next event
        }
        set_nonblocking(cfd);
        set_nodelay(cfd);
        set_nosigpipe(cfd);
        Connection* c = w.acquire(cfd);
#if defined(IRIS_HAVE_TLS)
        if (is_tls) {
            c->ssl = iris::net::tls::accept_on(cfd);
            if (c->ssl == nullptr) {
                w.release(c);
                continue;
            }
            c->state = CState::kTlsHandshake;
        }
#endif
        if (!w.poller.add(cfd, kReadable)) {
            w.release(c);
        }
    }
}

void worker_loop(Worker* wp, int cpu, bool pin) {
    Worker& w = *wp;
    if (pin) pin_to_cpu(cpu);

    PollEvent evs[Poller::kMaxEvents];
    for (;;) {
        int n = w.poller.wait(evs, Poller::kMaxEvents, -1);
        for (int i = 0; i < n; ++i) {
            int           fd = evs[i].fd;
            std::uint32_t fl = evs[i].flags;

            if (fd == w.listener
#if defined(IRIS_HAVE_TLS)
                || fd == w.tls_listener
#endif
            ) {
                on_accept(w, fd);
                continue;
            }

#if defined(IRIS_HAVE_IOURING)
            if (iou_active() && fd == iou_ring_fd(&w.iou)) {
                iou_reap(&w.iou, &w,
                         [](void* ctx, void* ud, int res) noexcept {
                             Worker&     wr = *static_cast<Worker*>(ctx);
                             Connection& c  = *static_cast<Connection*>(ud);
                             on_iou_send_done(wr, c, res);
                         });
                continue;
            }
#endif

#if defined(IRIS_HAVE_LIBPQ)
            if (w.db_enabled &&
                static_cast<std::size_t>(fd) < w.db_slot_by_fd.size() &&
                w.db_slot_by_fd[fd] >= 0) {
                on_db_event(w, w.db_slot_by_fd[fd], fl);
                continue;
            }
#endif

            Connection* c = w.find(fd);
            if (!c) continue;  // already released earlier in this batch

            bool ok = true;
            if (fl & kWritable) {
                ok = flush(w, *c);
                if (ok && c->wlen == 0 && c->state == CState::kActive && c->rlen > 0)
                    ok = drain(w, *c);
            }
            if (ok && (fl & kReadable)) {
                ok = on_readable(w, *c);
            }
            if (!ok) w.release(c);
        }
    }
}

}  // namespace

void AsyncCtx::set_zerocopy_body(const char* data, std::size_t len) noexcept {
    Connection* c = static_cast<Connection*>(conn_);
    if (!c || data == nullptr || len == 0) return;
    c->xbody = data;
    c->xfd   = -1;
    c->xoff  = 0;
    c->xlen  = len;
    c->xsent = 0;
}

void AsyncCtx::set_sendfile_response(int fd, std::size_t offset,
                                     std::size_t len) noexcept {
    Connection* c = static_cast<Connection*>(conn_);
    if (!c || fd < 0 || len == 0) return;
    c->xfd   = fd;
    c->xoff  = offset;
    c->xlen  = len;
    c->xsent = 0;
    c->xbody = nullptr;
    c->uring_inflight = false;
}

// AsyncCtx::run_db lives here so it can see the file-local Worker / Connection.
bool AsyncCtx::run_db(DbRoute route, int count) noexcept {
#if defined(IRIS_HAVE_LIBPQ)
    Worker*     w = static_cast<Worker*>(worker_);
    Connection* c = static_cast<Connection*>(conn_);
    if (!w || !c || !w->db_enabled) return false;  // DB off => handler 503s

    // Commit the connection to the async path: it is now suspended regardless of
    // whether a slot is free (if not, it queues and resumes when one frees).
    c->pend_route = route;
    c->pend_count = count;
    c->state      = CState::kAwaitDb;
    c->db_slot    = -1;

    // /db always goes through the batch queue: with an idle slot the kick
    // dispatches it immediately (batch of 1, no added latency); under load
    // queued requests coalesce into one pipeline per slot.
    if (route == DbRoute::kWorldOne) {
        w->db_one_queue.push_back({c, c->gen});
        db_kick_one_queue(*w);
        return true;
    }

    int slot = db_acquire(*w);
    if (slot < 0) {
        w->db_waiters.push_back({c, c->gen});  // pool busy: wait FIFO
        return true;
    }
    db_start_on_slot(*w, slot, c);
    return true;
#else
    (void)route;
    (void)count;
    return false;
#endif
}

#if defined(IRIS_WFB)
bool AsyncCtx::run_db_profile(std::string_view email) noexcept {
#if defined(IRIS_HAVE_LIBPQ)
    Worker*     w = static_cast<Worker*>(worker_);
    Connection* c = static_cast<Connection*>(conn_);
    if (!w || !c || !w->db_enabled || email.empty()) return false;

    const std::size_t n = email.size() < sizeof(c->pend_email) - 1
                              ? email.size()
                              : sizeof(c->pend_email) - 1;
    std::memcpy(c->pend_email, email.data(), n);
    c->pend_email[n]   = '\0';
    c->pend_email_len  = static_cast<int>(n);
    c->pend_route      = DbRoute::kUserProfile;
    c->pend_count      = 0;
    c->state           = CState::kAwaitDb;
    c->db_slot         = -1;

    int slot = db_acquire(*w);
    if (slot < 0) {
        w->db_waiters.push_back({c, c->gen});
        return true;
    }
    db_start_on_slot(*w, slot, c);
    return true;
#else
    (void)email;
    return false;
#endif
}
#endif  // IRIS_WFB

#if defined(IRIS_HA)
bool AsyncCtx::run_ha_async_db(int min_price, int max_price, int limit) noexcept {
#if defined(IRIS_HAVE_LIBPQ)
    Worker*     w = static_cast<Worker*>(worker_);
    Connection* c = static_cast<Connection*>(conn_);
    if (!w || !c || !w->db_enabled) return false;

    if (limit < 1) limit = 1;
    if (limit > 50) limit = 50;

    c->pend_route  = DbRoute::kHaAsyncDb;
    c->ha_min      = min_price;
    c->ha_max      = max_price;
    c->pend_count  = limit;
    c->state       = CState::kAwaitDb;
    c->db_slot     = -1;

    int slot = db_acquire(*w);
    if (slot < 0) {
        w->db_waiters.push_back({c, c->gen});
        return true;
    }
    db_start_on_slot(*w, slot, c);
    return true;
#else
    (void)min_price;
    (void)max_price;
    (void)limit;
    return false;
#endif
}
#endif  // IRIS_HA

int run_server(const ServerConfig& cfg, Handler handler) noexcept {
    ::signal(SIGPIPE, SIG_IGN);
    iris::http::start_date_clock();

#if defined(IRIS_HAVE_TLS)
    if (cfg.tls_port > 0 && cfg.tls_cert != nullptr && cfg.tls_key != nullptr) {
        if (!iris::net::tls::init(cfg.tls_cert, cfg.tls_key)) {
            std::fprintf(stderr, "[iris-gw] TLS init failed\n");
            return 1;
        }
    }
#endif

#if defined(IRIS_HAVE_IOURING)
    if (cfg.iou_blob != nullptr && cfg.iou_blob_len > 0) {
        iou_bind_region(cfg.iou_blob, cfg.iou_blob_len);
        std::fprintf(stderr, "[iris-gw] io_uring static blob: %zu bytes (%s)\n",
                     cfg.iou_blob_len,
                     iou_active() ? "registered" : "bind failed");
    }
#endif

    int workers = cfg.workers > 0
                      ? cfg.workers
                      : static_cast<int>(std::thread::hardware_concurrency());
    if (workers < 1) workers = 1;

    bool reuseport = cfg.reuseport;
#if !defined(SO_REUSEPORT)
    reuseport = false;
#endif
    if (!reuseport) workers = 1;

    std::vector<Worker*>     ws;
    std::vector<std::thread> threads;
    ws.reserve(workers);

    for (int i = 0; i < workers; ++i) {
        Worker* w  = new Worker();
        w->handler = handler;
        w->rcap    = cfg.read_cap;
        w->wcap    = cfg.write_cap;
        if (!w->poller.ok()) {
            std::fprintf(stderr, "[iris-gw] poller create failed\n");
            return 1;
        }
        int lfd = make_listener(cfg.port, reuseport, cfg.backlog);
        if (lfd < 0) {
            std::fprintf(stderr, "[iris-gw] listen on :%u failed: %s\n",
                         static_cast<unsigned>(cfg.port), std::strerror(errno));
            return 1;
        }
        w->listener = lfd;
        w->poller.add(lfd, kReadable);

#if defined(IRIS_HAVE_TLS)
        if (cfg.tls_port > 0 && iris::net::tls::enabled()) {
            int tfd = make_listener(cfg.tls_port, reuseport, cfg.backlog);
            if (tfd < 0) {
                std::fprintf(stderr, "[iris-gw] TLS listen on :%u failed: %s\n",
                             static_cast<unsigned>(cfg.tls_port),
                             std::strerror(errno));
                return 1;
            }
            w->tls_listener = tfd;
            w->poller.add(tfd, kReadable);
        }
#endif

#if defined(IRIS_HAVE_IOURING)
        if (iou_active()) {
            if (!iou_worker_init(&w->iou)) {
                std::fprintf(stderr,
                             "[iris-gw] worker %d io_uring init failed; "
                             "static send falls back to sendfile\n",
                             i);
            } else {
                w->poller.add(iou_ring_fd(&w->iou), kReadable);
            }
        }
#endif

#if defined(IRIS_HAVE_LIBPQ)
        if (cfg.db.conninfo != nullptr) {
            w->fmt         = cfg.db.fmt;
            w->db_conninfo = cfg.db.conninfo;
            const int n    = cfg.db.pool_per_worker > 0 ? cfg.db.pool_per_worker : 1;
            w->db_conns.resize(n);
            w->db_jobs.resize(n);
            for (int s = 0; s < n; ++s) {
                db::PgConn& pc = w->db_conns[s];
                if (!pc.connect(cfg.db.conninfo) ||
#if defined(IRIS_HA)
                    !pc.prepare(kStmtHaAsyncDb, kSqlHaAsyncDb, 3)
#else
                    !pc.prepare(kStmtWorldSelect, kSqlWorldSelect, 1) ||
                    !pc.prepare(kStmtFortuneAll, kSqlFortuneAll, 0) ||
                    !pc.prepare(kStmtWorldBulk, kSqlWorldBulk, 2)
#if defined(IRIS_WFB)
                    || !pc.prepare(kStmtWfbUser, kSqlWfbUser, 1) ||
                    !pc.prepare(kStmtWfbTrending, kSqlWfbTrending, 0) ||
                    !pc.prepare(kStmtWfbUpdate, kSqlWfbUpdate, 1) ||
                    !pc.prepare(kStmtWfbPosts, kSqlWfbPosts, 1)
#endif
#endif
                    ) {
                    std::fprintf(stderr, "[iris-gw] worker %d DB slot %d bringup "
                                 "failed: %s\n", i, s, pc.error().c_str());
                    return 1;
                }
                int dfd = pc.socket();
                if (static_cast<std::size_t>(dfd) >= w->db_slot_by_fd.size())
                    w->db_slot_by_fd.resize(dfd + 1, -1);
                w->db_slot_by_fd[dfd] = s;
                w->poller.add(dfd, kReadable);
                w->db_idle.push_back(s);
            }
            w->db_enabled = true;
        }
#endif
        ws.push_back(w);
    }

#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
    // Steer each incoming connection to the listener whose index matches the
    // CPU that handled the SYN (cpu % workers). With workers pinned to CPU i,
    // a connection is served on the core that already owns its RX queue --
    // aligning SO_REUSEPORT with RSS/IRQ steering instead of the default
    // 4-tuple hash. Attached once: the program applies to the whole group.
    if (reuseport && workers > 1 && cfg.pin_threads) {
        struct sock_filter code[] = {
            {BPF_LD | BPF_W | BPF_ABS, 0, 0,
             static_cast<std::uint32_t>(SKF_AD_OFF + SKF_AD_CPU)},
            {BPF_ALU | BPF_MOD | BPF_K, 0, 0,
             static_cast<std::uint32_t>(workers)},
            {BPF_RET | BPF_A, 0, 0, 0},
        };
        struct sock_fprog prog = {3, code};
        if (::setsockopt(ws.back()->listener, SOL_SOCKET,
                         SO_ATTACH_REUSEPORT_CBPF, &prog, sizeof(prog)) == 0) {
            std::printf("[iris-gw] reuseport CBPF: cpu-affine steering (%d)\n",
                        workers);
        } else {
            std::fprintf(stderr, "[iris-gw] reuseport CBPF unavailable (%s); "
                         "kernel hash steering kept\n", std::strerror(errno));
        }
        std::fflush(stdout);
    }
#endif

    for (int i = 0; i < workers; ++i) {
        threads.emplace_back(worker_loop, ws[i], i, cfg.pin_threads);
    }
    for (auto& t : threads) t.join();
    return 0;
}

}  // namespace iris::net
