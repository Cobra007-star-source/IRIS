// =============================================================================
// src/net/server.cpp
//
// Thread-per-core HTTP/1.1 event loop.
//
//   * One worker per core; each owns a Poller, a SO_REUSEPORT listener (racing
//     profile), and a pool of Connections with fixed read/write buffers.
//   * Level-triggered readiness. Reads drain the socket, parse every pipelined
//     request, and batch all responses into one write buffer flushed with a
//     single send() (writev is reserved for the later scatter-gather body path).
//   * Zero allocation in steady state: connection objects and their buffers are
//     recycled through a per-worker free list.
//
// Backpressure / pipelining correctness: request views point into the read
// buffer, so the buffer is only compacted after a request's response has been
// produced, or just before returning when a mid-batch flush blocks (in which
// case the still-unprocessed bytes are preserved at the front and reprocessed
// when the socket becomes writable again).
// =============================================================================
#include "iris/net/server.hpp"

#include "iris/net/poller.hpp"
#include "iris/net/socket.hpp"
#include "iris/http/date.hpp"
#include "iris/http/parser.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
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
// before invoking the handler. TFB responses are < 300 bytes.
constexpr std::size_t kRespReserve = 2048;

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

struct Connection {
    int         fd                = -1;
    bool        want_write        = false;
    bool        close_after_flush = false;
    std::size_t rlen  = 0;   // valid bytes in rbuf
    std::size_t wlen  = 0;   // valid bytes in wbuf
    std::size_t wsent = 0;   // bytes of wbuf already written
    std::size_t rcap  = 0;
    std::size_t wcap  = 0;
    char*       rbuf  = nullptr;
    char*       wbuf  = nullptr;
};

struct Worker {
    Poller                    poller;
    int                       listener = -1;
    Handler                   handler  = nullptr;
    std::size_t               rcap     = 4096;
    std::size_t               wcap     = 32768;
    std::vector<Connection*>  by_fd;
    std::vector<Connection*>  freelist;

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
        c->want_write = false;
        c->close_after_flush = false;
        c->rlen = c->wlen = c->wsent = 0;
        if (static_cast<std::size_t>(fd) >= by_fd.size()) by_fd.resize(fd + 1, nullptr);
        by_fd[fd] = c;
        return c;
    }

    void release(Connection* c) {
        if (c->fd >= 0) {
            poller.del(c->fd);
            ::close(c->fd);
            if (static_cast<std::size_t>(c->fd) < by_fd.size()) by_fd[c->fd] = nullptr;
            c->fd = -1;
        }
        freelist.push_back(c);
    }

    Connection* find(int fd) const {
        return (static_cast<std::size_t>(fd) < by_fd.size()) ? by_fd[fd] : nullptr;
    }
};

// Flush pending [wsent, wlen). Returns false to close the connection (fatal
// error, or a graceful close requested after the final byte is sent).
bool flush(Worker& w, Connection& c) {
    while (c.wsent < c.wlen) {
        ssize_t n = ::send(c.fd, c.wbuf + c.wsent, c.wlen - c.wsent, MSG_NOSIGNAL);
        if (n > 0) {
            c.wsent += static_cast<std::size_t>(n);
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
    if (c.want_write) {
        w.poller.mod(c.fd, kReadable);
        c.want_write = false;
    }
    return !c.close_after_flush;
}

// Parse and respond to every complete (pipelined) request currently buffered.
bool drain(Worker& w, Connection& c) {
    std::size_t off = 0;
    while (off < c.rlen) {
        iris::http::Request req;
        auto pr = iris::http::parse_request(c.rbuf + off, c.rlen - off, req);
        if (pr.status == iris::http::ParseStatus::kIncomplete) break;
        if (pr.status == iris::http::ParseStatus::kError) return false;

        // Guarantee room for this response before touching the handler.
        if (c.wcap - c.wlen < kRespReserve) {
            if (!flush(w, c)) return false;
            if (c.want_write) {
                // Could not free space (write blocked). Preserve unprocessed
                // input [off, rlen) at the front; resume when writable. `req`
                // becomes invalid here but we return without using it.
                std::memmove(c.rbuf, c.rbuf + off, c.rlen - off);
                c.rlen -= off;
                return true;
            }
        }

        iris::http::Buffer ob(c.wbuf, c.wcap, c.wlen);
        w.handler(req, ob);
        c.wlen = ob.size();
        off += pr.consumed;

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
    for (;;) {
        if (c.rlen == c.rcap) break;  // buffer full: drain to free space
        ssize_t n = ::recv(c.fd, c.rbuf + c.rlen, c.rcap - c.rlen, 0);
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
    return drain(w, c);
}

void on_accept(Worker& w) {
    for (;;) {
        int cfd = ::accept(w.listener, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;  // transient (e.g. EMFILE): stop this round, retry next event
        }
        set_nonblocking(cfd);
        set_nodelay(cfd);
        set_nosigpipe(cfd);
        Connection* c = w.acquire(cfd);
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

            if (fd == w.listener) {
                on_accept(w);
                continue;
            }

            Connection* c = w.find(fd);
            if (!c) continue;  // already released earlier in this batch

            bool ok = true;
            if (fl & kWritable) {
                ok = flush(w, *c);
                if (ok && c->wlen == 0 && c->rlen > 0) ok = drain(w, *c);
            }
            if (ok && (fl & kReadable)) {
                ok = on_readable(w, *c);
            }
            if (!ok) w.release(c);
        }
    }
}

}  // namespace

int run_server(const ServerConfig& cfg, Handler handler) noexcept {
    ::signal(SIGPIPE, SIG_IGN);
    iris::http::start_date_clock();

    int workers = cfg.workers > 0
                      ? cfg.workers
                      : static_cast<int>(std::thread::hardware_concurrency());
    if (workers < 1) workers = 1;

    bool reuseport = cfg.reuseport;
#if !defined(SO_REUSEPORT)
    reuseport = false;
#endif
    // Without SO_REUSEPORT we cannot give each worker its own listener safely,
    // so fall back to a single acceptor thread.
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
        ws.push_back(w);
    }

    for (int i = 0; i < workers; ++i) {
        threads.emplace_back(worker_loop, ws[i], i, cfg.pin_threads);
    }
    for (auto& t : threads) t.join();
    return 0;
}

}  // namespace iris::net
