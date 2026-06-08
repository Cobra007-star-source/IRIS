// =============================================================================
// src/net/poller.cpp
//
// epoll (Linux) and kqueue (macOS/BSD) backends for iris::net::Poller.
// =============================================================================
#include "iris/net/poller.hpp"

#include <cerrno>
#include <ctime>
#include <unistd.h>

namespace iris::net {

#if defined(__linux__)

Poller::Poller() { pfd_ = ::epoll_create1(EPOLL_CLOEXEC); }
Poller::~Poller() { if (pfd_ >= 0) ::close(pfd_); }

namespace {
inline std::uint32_t to_epoll(std::uint32_t f) noexcept {
    std::uint32_t e = 0;
    if (f & kReadable) e |= EPOLLIN;
    if (f & kWritable) e |= EPOLLOUT;
    return e;
}
}  // namespace

bool Poller::add(int fd, std::uint32_t f) noexcept {
    epoll_event ev{};
    ev.events  = to_epoll(f);
    ev.data.fd = fd;
    return ::epoll_ctl(pfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Poller::mod(int fd, std::uint32_t f) noexcept {
    epoll_event ev{};
    ev.events  = to_epoll(f);
    ev.data.fd = fd;
    return ::epoll_ctl(pfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Poller::del(int fd) noexcept {
    return ::epoll_ctl(pfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Poller::wait(PollEvent* out, int max, int timeout_ms) noexcept {
    int lim = max < kMaxEvents ? max : kMaxEvents;
    int n   = ::epoll_wait(pfd_, evbuf_, lim, timeout_ms);
    if (n < 0) return 0;
    for (int i = 0; i < n; ++i) {
        std::uint32_t e = evbuf_[i].events;
        std::uint32_t f = 0;
        // Surface HUP/ERR as readable so the next recv() observes the EOF/error.
        if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) f |= kReadable;
        if (e & EPOLLOUT)                        f |= kWritable;
        out[i].fd    = evbuf_[i].data.fd;
        out[i].flags = f;
    }
    return n;
}

#elif defined(IRIS_NET_KQUEUE)

Poller::Poller() { pfd_ = ::kqueue(); }
Poller::~Poller() { if (pfd_ >= 0) ::close(pfd_); }

bool Poller::add(int fd, std::uint32_t f) noexcept { return mod(fd, f); }

bool Poller::mod(int fd, std::uint32_t f) noexcept {
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ,
           EV_ADD | ((f & kReadable) ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
    EV_SET(&ch[1], fd, EVFILT_WRITE,
           EV_ADD | ((f & kWritable) ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
    return ::kevent(pfd_, ch, 2, nullptr, 0, nullptr) == 0;
}

bool Poller::del(int fd) noexcept {
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(pfd_, ch, 2, nullptr, 0, nullptr);  // ignore ENOENT for absent filters
    return true;
}

int Poller::wait(PollEvent* out, int max, int timeout_ms) noexcept {
    struct timespec  ts;
    struct timespec* pts = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        pts        = &ts;
    }
    int lim = max < kMaxEvents ? max : kMaxEvents;
    int n   = ::kevent(pfd_, nullptr, 0, evbuf_, lim, pts);
    if (n < 0) return 0;
    for (int i = 0; i < n; ++i) {
        std::uint32_t f = 0;
        if (evbuf_[i].filter == EVFILT_READ)  f |= kReadable;
        if (evbuf_[i].filter == EVFILT_WRITE) f |= kWritable;
        if (evbuf_[i].flags & EV_EOF)         f |= kReadable;  // let recv() see EOF
        out[i].fd    = static_cast<int>(evbuf_[i].ident);
        out[i].flags = f;
    }
    return n;
}

#else
#error "iris::net::Poller: unsupported platform (need epoll or kqueue)"
#endif

}  // namespace iris::net
