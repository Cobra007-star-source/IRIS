// =============================================================================
// iris/net/poller.hpp
//
// Minimal readiness-notification abstraction over epoll (Linux) and kqueue
// (macOS/BSD). One Poller is owned per worker thread; it is never shared, so
// it carries no locking.
// =============================================================================
#pragma once

#include <cstdint>

#if defined(__linux__)
    #include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__)
    #include <sys/event.h>
    #define IRIS_NET_KQUEUE 1
#endif

namespace iris::net {

enum EventFlags : std::uint32_t {
    kReadable = 1u << 0,
    kWritable = 1u << 1,
};

struct PollEvent {
    int           fd;
    std::uint32_t flags;
};

class Poller {
public:
    static constexpr int kMaxEvents = 1024;

    Poller();
    ~Poller();
    Poller(const Poller&)            = delete;
    Poller& operator=(const Poller&) = delete;

    [[nodiscard]] bool ok() const noexcept { return pfd_ >= 0; }

    bool add(int fd, std::uint32_t flags) noexcept;
    bool mod(int fd, std::uint32_t flags) noexcept;
    bool del(int fd) noexcept;

    // Block up to timeout_ms (-1 = forever); fill `out` with up to `max` ready
    // events and return the count.
    int wait(PollEvent* out, int max, int timeout_ms) noexcept;

private:
    int pfd_ = -1;
#if defined(__linux__)
    struct epoll_event evbuf_[kMaxEvents];
#elif defined(IRIS_NET_KQUEUE)
    struct kevent      evbuf_[kMaxEvents];
#endif
};

}  // namespace iris::net
