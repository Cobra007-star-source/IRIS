// =============================================================================
// iris/net/socket.hpp
//
// Small POSIX socket helpers shared by the event loop. Inline where trivial;
// make_listener() is defined in src/net/server.cpp.
// =============================================================================
#pragma once

#include <cstdint>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace iris::net {

inline bool set_nonblocking(int fd) noexcept {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

inline void set_nodelay(int fd) noexcept {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// On Apple/BSD, suppress SIGPIPE per-socket (Linux uses MSG_NOSIGNAL on send).
inline void set_nosigpipe(int fd) noexcept {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

// Create a non-blocking listening socket bound to `port`. When `reuseport` is
// set, SO_REUSEPORT lets each worker own an independent listener for the same
// port so the kernel load-balances accepts (no shared accept lock).
// Returns -1 on failure (errno set).
int make_listener(std::uint16_t port, bool reuseport, int backlog) noexcept;

}  // namespace iris::net
