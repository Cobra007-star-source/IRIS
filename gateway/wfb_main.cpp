// =============================================================================
// gateway/wfb_main.cpp
//
// Web Framework Benchmark gateway. Routes:
//   GET  /health
//   GET  /plaintext
//   POST /json/aggregate
//   GET  /db/user-profile/:email   (when DB configured)
//   GET/HEAD /files/15kb.bin, /files/1mb.bin
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "iris/db/pg.hpp"
#include "iris/http/buffer.hpp"
#include "iris/http/date.hpp"
#include "iris/http/response.hpp"
#include "iris/net/server.hpp"
#include "wfb_aggregate.hpp"
#include "wfb_static.hpp"

namespace {

using iris::http::Buffer;
using iris::http::Request;
using iris::net::AsyncCtx;
using iris::net::Outcome;

constexpr std::string_view kHelloPlain = "Hello, World!";
constexpr std::size_t       kDateLen   = 29;
constexpr std::string_view kProfPrefix = "/db/user-profile/";

const char* g_db_conninfo = nullptr;
bool        g_db_enabled  = false;

struct RespTemplate {
    std::string bytes;
    std::size_t date_off = 0;
};

RespTemplate g_plain;

RespTemplate build_plain_template() {
    RespTemplate t;
    std::string& b = t.bytes;
    b  = "HTTP/1.1 200 OK\r\nServer: ";
    b += iris::http::kServerName;
    b += "\r\nDate: ";
    t.date_off = b.size();
    b.append(kDateLen, '?');
    b += "\r\nContent-Type: text/plain\r\nContent-Length: ";
    b += std::to_string(kHelloPlain.size());
    b += "\r\n\r\n";
    b.append(kHelloPlain.data(), kHelloPlain.size());
    return t;
}

inline void emit_plain(Buffer& out) noexcept {
    const std::size_t base = out.size();
    out.append(std::string_view(g_plain.bytes));
    if (!out.overflow()) {
        std::memcpy(out.data() + base + g_plain.date_off,
                    iris::http::current_date().data(), kDateLen);
    }
}

inline bool path_is(std::string_view path, std::string_view route) noexcept {
    return path == route || (path.size() > route.size() &&
                           path.substr(0, route.size()) == route &&
                           path[route.size()] == '?');
}

std::string build_wfb_conninfo() {
    const char* host = std::getenv("DB_HOST");
    if (host == nullptr || host[0] == '\0') {
        const char* iris = std::getenv("IRIS_DB");
        return iris ? std::string(iris) : std::string();
    }
    const char* user = std::getenv("DB_USER");
    const char* pass = std::getenv("DB_PASSWORD");
    const char* port = std::getenv("DB_PORT");
    const char* name = std::getenv("DB_NAME");
    if (user == nullptr) user = "benchmark";
    if (pass == nullptr) pass = "benchmark";
    if (port == nullptr) port = "5432";
    if (name == nullptr) name = "benchmark";
    return std::string("host=") + host + " port=" + port + " user=" + user +
           " password=" + pass + " dbname=" + name;
}

Outcome handle_health(Buffer& out, const Request& req) noexcept {
    if (g_db_enabled && g_db_conninfo != nullptr) {
        if (!iris::db::ping(g_db_conninfo)) {
            iris::http::write_response(out, 503, "Service Unavailable",
                                       "text/plain", "db error", req.minor_version,
                                       false);
            return Outcome::kResponded;
        }
    }
    iris::http::write_response(out, 200, "OK", "text/plain", "OK",
                               req.minor_version, req.keep_alive);
    return Outcome::kResponded;
}

Outcome handle_profile(const Request& req, Buffer& out, AsyncCtx& ctx) noexcept {
    if (!g_db_enabled) {
        iris::http::write_response(out, 503, "Service Unavailable", "text/plain",
                                   "db unavailable", req.minor_version, false);
        return Outcome::kResponded;
    }
    std::string_view path = req.path;
    if (path.size() <= kProfPrefix.size()) {
        iris::http::write_response(out, 404, "Not Found", "text/plain", "",
                                   req.minor_version, req.keep_alive);
        return Outcome::kResponded;
    }
    std::string_view email = path.substr(kProfPrefix.size());
    const std::size_t q = email.find('?');
    if (q != std::string_view::npos) email = email.substr(0, q);
    if (email.empty()) {
        iris::http::write_response(out, 404, "Not Found", "text/plain", "",
                                   req.minor_version, req.keep_alive);
        return Outcome::kResponded;
    }
    if (ctx.run_db_profile(email)) return Outcome::kSuspended;
    iris::http::write_response(out, 503, "Service Unavailable", "text/plain",
                               "db busy", req.minor_version, false);
    return Outcome::kResponded;
}

Outcome handle(const Request& req, Buffer& out, AsyncCtx& ctx) noexcept {
    if (req.method == "GET" && path_is(req.path, "/health")) {
        return handle_health(out, req);
    }
    if (req.method == "GET" &&
        (req.path == "/plaintext" || req.path == "/")) {
        if (req.keep_alive && req.minor_version >= 1) {
            emit_plain(out);
            return Outcome::kResponded;
        }
        iris::http::write_response(out, 200, "OK", "text/plain", kHelloPlain,
                                   req.minor_version, req.keep_alive);
        return Outcome::kResponded;
    }
    if (req.method == "POST" && req.path == "/json/aggregate") {
        char body_buf[131072];
        Buffer body_out(body_buf, sizeof(body_buf));
        iris::wfb::json_aggregate(req.body, body_out);
        iris::http::write_response(out, 200, "OK", "application/json",
                                   body_out.view(), req.minor_version,
                                   req.keep_alive);
        return Outcome::kResponded;
    }
    if ((req.method == "GET" || req.method == "HEAD") &&
        req.path.rfind("/files/", 0) == 0) {
        if (iris::wfb::handle_static(req, out, req.minor_version, req.keep_alive)) {
            return Outcome::kResponded;
        }
    }
    if (req.method == "GET" && req.path.rfind(kProfPrefix, 0) == 0) {
        return handle_profile(req, out, ctx);
    }
    iris::http::write_response(out, 404, "Not Found", "text/plain", "",
                               req.minor_version, req.keep_alive);
    return Outcome::kResponded;
}

}  // namespace

int main(int argc, char** argv) {
    iris::net::ServerConfig cfg;
    cfg.read_cap  = 262144;
    cfg.write_cap = 131072;

    std::string db_owned;
    int         db_pool = 8;

    const char* data_dir = std::getenv("DATA_DIR");
    if (data_dir == nullptr) data_dir = "benchmarks_data";

    if (const char* port_env = std::getenv("PORT")) {
        cfg.port = static_cast<std::uint16_t>(std::atoi(port_env));
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            cfg.workers = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-pin") == 0) {
            cfg.pin_threads = false;
        } else if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_owned = argv[++i];
        } else if (std::strcmp(argv[i], "--db-pool") == 0 && i + 1 < argc) {
            db_pool = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: iris-wfb-gw [--port N] [--workers N] [--data-dir PATH]"
                " [--db CONNINFO] [--db-pool N]\n");
            return 0;
        }
    }

    if (db_owned.empty()) db_owned = build_wfb_conninfo();
    if (!db_owned.empty()) {
        g_db_conninfo        = db_owned.c_str();
        g_db_enabled         = true;
        cfg.db.conninfo      = g_db_conninfo;
        if (const char* ps = std::getenv("DB_POOL_SIZE")) {
            db_pool = std::atoi(ps);
        }
        cfg.db.pool_per_worker = db_pool > 0 ? db_pool : 1;
    }

    if (!iris::wfb::load_static_files(data_dir)) {
        std::fprintf(stderr,
                     "[iris-wfb-gw] static files missing in %s (15kb.bin, 1mb.bin)\n",
                     data_dir);
        return 1;
    }

    g_plain = build_plain_template();

    std::printf("[iris-wfb-gw] listening on :%u data=%s db=%s\n",
                static_cast<unsigned>(cfg.port), data_dir,
                g_db_enabled ? "on" : "off");
    std::fflush(stdout);

    return iris::net::run_server(cfg, handle);
}
