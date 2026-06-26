// =============================================================================
// src/net/tls.cpp
// =============================================================================
#include "iris/net/tls.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace iris::net::tls {

namespace {

SSL_CTX* g_ctx = nullptr;

}  // namespace

bool init(const char* cert_path, const char* key_path) noexcept {
    if (cert_path == nullptr || key_path == nullptr) return false;
    if (g_ctx != nullptr) return true;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                         OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                     nullptr);
#else
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    g_ctx = SSL_CTX_new(TLS_server_method());
    if (g_ctx == nullptr) return false;

    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(g_ctx, SSL_OP_NO_COMPRESSION);

    // HttpArena json-tls: ALPN advertises http/1.1 only.
    static const unsigned char kAlpn[] = {
        8, 'h', 't', 't', 'p', '/', '1', '.', '1',
    };
    if (SSL_CTX_set_alpn_protos(g_ctx, kAlpn, sizeof(kAlpn)) != 0) {
        SSL_CTX_free(g_ctx);
        g_ctx = nullptr;
        return false;
    }

    if (SSL_CTX_use_certificate_file(g_ctx, cert_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(g_ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(g_ctx) != 1) {
        SSL_CTX_free(g_ctx);
        g_ctx = nullptr;
        return false;
    }

    SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_SERVER);
    return true;
}

void fini() noexcept {
    if (g_ctx != nullptr) {
        SSL_CTX_free(g_ctx);
        g_ctx = nullptr;
    }
}

bool enabled() noexcept { return g_ctx != nullptr; }

ssl_st* accept_on(int fd) noexcept {
    if (g_ctx == nullptr || fd < 0) return nullptr;
    SSL* ssl = SSL_new(g_ctx);
    if (ssl == nullptr) return nullptr;
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    return ssl;
}

void free_conn(ssl_st* ssl) noexcept {
    if (ssl == nullptr) return;
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

Handshake handshake(ssl_st* ssl) noexcept {
    if (ssl == nullptr) return Handshake::kError;
    const int r = SSL_accept(ssl);
    if (r == 1) return Handshake::kDone;
    const int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ) return Handshake::kWantRead;
    if (err == SSL_ERROR_WANT_WRITE) return Handshake::kWantWrite;
    return Handshake::kError;
}

ssize_t read(ssl_st* ssl, int fd, char* buf, std::size_t len) noexcept {
    if (ssl == nullptr) {
        return ::recv(fd, buf, len, 0);
    }
    for (;;) {
        const int n = SSL_read(ssl, buf, static_cast<int>(len));
        if (n > 0) return n;
        if (n == 0) return 0;
        const int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
            return -1;
        }
        if (err == SSL_ERROR_SYSCALL && (errno == EINTR)) continue;
        return -1;
    }
}

ssize_t write(ssl_st* ssl, int fd, const char* buf, std::size_t len) noexcept {
    if (ssl == nullptr) {
        return ::send(fd, buf, len, MSG_NOSIGNAL);
    }
    for (;;) {
        const int n = SSL_write(ssl, buf, static_cast<int>(len));
        if (n > 0) return n;
        const int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            errno = EAGAIN;
            return -1;
        }
        if (err == SSL_ERROR_SYSCALL && (errno == EINTR)) continue;
        return -1;
    }
}

}  // namespace iris::net::tls
