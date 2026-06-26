// =============================================================================
// iris/net/tls.hpp
//
// Optional OpenSSL server-side TLS for a second HTTP/1.1 listener (HttpArena
// json-tls on :8081). Process-global SSL_CTX initialized once; per-connection
// SSL* handles non-blocking handshakes and record I/O.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

struct ssl_st;

namespace iris::net::tls {

enum class Handshake : std::uint8_t {
    kDone = 0,
    kWantRead,
    kWantWrite,
    kError,
};

[[nodiscard]] bool init(const char* cert_path, const char* key_path) noexcept;
void               fini() noexcept;
[[nodiscard]] bool enabled() noexcept;

[[nodiscard]] ssl_st* accept_on(int fd) noexcept;
void                free_conn(ssl_st* ssl) noexcept;

[[nodiscard]] Handshake handshake(ssl_st* ssl) noexcept;

[[nodiscard]] ssize_t read(ssl_st* ssl, int fd, char* buf, std::size_t len) noexcept;
[[nodiscard]] ssize_t write(ssl_st* ssl, int fd, const char* buf,
                             std::size_t len) noexcept;

}  // namespace iris::net::tls
