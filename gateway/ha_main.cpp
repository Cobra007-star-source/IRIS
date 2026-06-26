// =============================================================================
// gateway/ha_main.cpp
//
// HTTPArena gateway. Routes the isolated HTTP/1.1 profiles onto the
// thread-per-core server core:
//
//   GET/POST /baseline11?a=&b=  -> text/plain sum of query params (+ POST body)
//   GET      /pipeline          -> text/plain "ok"
//   GET      /json/{count}?m=N  -> application/json items with computed totals
//   GET/HEAD /static/<name>     -> cached static file with correct Content-Type
//
// Hot-path strategy mirrors the TFB gateway: /pipeline is served from a byte
// template with a fixed-offset Date slot (memcpy + 29-byte patch, no per-request
// formatting). /baseline11 formats only the small integer body. /json and
// /static reuse precomputed per-item / per-file templates.
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "ha_json.hpp"
#include "ha_static.hpp"
#include "iris/http/buffer.hpp"
#include "iris/http/date.hpp"
#include "iris/http/response.hpp"
#include "iris/net/server.hpp"

namespace {

using iris::http::Buffer;
using iris::http::Request;
using iris::net::AsyncCtx;
using iris::net::Outcome;

constexpr std::string_view kOk      = "ok";
constexpr std::size_t       kDateLen = 29;

struct RespTemplate {
    std::string bytes;
    std::size_t date_off = 0;
};

RespTemplate g_pipeline;

RespTemplate build_template(std::string_view content_type, std::string_view body) {
    RespTemplate t;
    std::string& b = t.bytes;
    b  = "HTTP/1.1 200 OK\r\nServer: iris\r\nDate: ";
    t.date_off = b.size();
    b.append(kDateLen, '?');
    b += "\r\nContent-Type: ";
    b += content_type;
    b += "\r\nContent-Length: ";
    b += std::to_string(body.size());
    b += "\r\n\r\n";
    b.append(body.data(), body.size());
    return t;
}

inline void emit_template(Buffer& out, const RespTemplate& t) noexcept {
    const std::size_t base = out.size();
    out.append(std::string_view(t.bytes));
    if (!out.overflow()) {
        std::memcpy(out.data() + base + t.date_off,
                    iris::http::current_date().data(), kDateLen);
    }
}

inline bool path_is(std::string_view path, std::string_view route) noexcept {
    return path == route || (path.size() > route.size() &&
                             path.substr(0, route.size()) == route &&
                             path[route.size()] == '?');
}

// Parse the first signed integer at the front of `s` (after leading spaces).
long parse_int(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
    long v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        any = true;
        ++i;
    }
    if (!any) return 0;
    return neg ? -v : v;
}

// Sum every numeric value in the query string of `path` (the part after '?').
long sum_query(std::string_view path) noexcept {
    const std::size_t q = path.find('?');
    if (q == std::string_view::npos) return 0;
    std::string_view qs = path.substr(q + 1);
    long sum = 0;
    while (!qs.empty()) {
        const std::size_t amp = qs.find('&');
        std::string_view tok = amp == std::string_view::npos ? qs : qs.substr(0, amp);
        const std::size_t eq = tok.find('=');
        if (eq != std::string_view::npos) sum += parse_int(tok.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        qs = qs.substr(amp + 1);
    }
    return sum;
}

void emit_text_int(Buffer& out, long value, const Request& req) noexcept {
    char tmp[24];
    int n = std::snprintf(tmp, sizeof(tmp), "%ld", value);
    iris::http::write_response(out, 200, "OK", "text/plain",
                               std::string_view(tmp, static_cast<std::size_t>(n)),
                               req.minor_version, req.keep_alive);
}

// Parse "/json/{count}" and the "m" query param. Returns false if malformed.
bool parse_json_path(std::string_view path, std::size_t& count,
                     long& multiplier) noexcept {
    std::string_view rest = path;
    rest.remove_prefix(std::strlen("/json/"));
    std::size_t i = 0;
    count = 0;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') {
        count = count * 10 + static_cast<std::size_t>(rest[i] - '0');
        ++i;
    }
    if (i == 0) return false;
    multiplier = 1;
    const std::size_t q = path.find("m=");
    if (q != std::string_view::npos) multiplier = parse_int(path.substr(q + 2));
    return true;
}

Outcome handle(const Request& req, Buffer& out, AsyncCtx& ctx) {
    const std::string_view path = req.path;

    if (path == "/pipeline") {
        if (req.keep_alive && req.minor_version >= 1) {
            emit_template(out, g_pipeline);
        } else {
            iris::http::write_response(out, 200, "OK", "text/plain", kOk,
                                       req.minor_version, req.keep_alive);
        }
        return Outcome::kResponded;
    }

    if (path_is(path, "/baseline11")) {
        long sum = sum_query(path);
        if (req.method == "POST" && !req.body.empty()) sum += parse_int(req.body);
        emit_text_int(out, sum, req);
        return Outcome::kResponded;
    }

    if (path.rfind("/json/", 0) == 0) {
        std::size_t count = 0;
        long        m     = 1;
        if (parse_json_path(path, count, m) && iris::ha::dataset_size() > 0) {
            static thread_local char body_buf[262144];
            Buffer body(body_buf, sizeof(body_buf));
            iris::ha::serialize_json(body, count, m);
            iris::http::write_response(out, 200, "OK", "application/json",
                                       body.view(), req.minor_version,
                                       req.keep_alive);
        } else {
            iris::http::write_response(out, 503, "Service Unavailable",
                                       "text/plain", "dataset unavailable",
                                       req.minor_version, req.keep_alive);
        }
        return Outcome::kResponded;
    }

    if (path.rfind("/static/", 0) == 0) {
        iris::ha::handle_static(req, out, ctx, req.minor_version, req.keep_alive);
        return Outcome::kResponded;
    }

    iris::http::write_response(out, 404, "Not Found", "text/plain", "",
                               req.minor_version, req.keep_alive);
    return Outcome::kResponded;
}

}  // namespace

int main(int argc, char** argv) {
    iris::net::ServerConfig cfg;
    cfg.read_cap  = 65536;
    cfg.write_cap = 262144;

    const char* dataset = std::getenv("DATASET_PATH");
    const char* static_dir = std::getenv("STATIC_DIR");
    if (dataset == nullptr)    dataset = "/data/dataset.json";
    if (static_dir == nullptr) static_dir = "/data/static";

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
        } else if (std::strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) {
            dataset = argv[++i];
        } else if (std::strcmp(argv[i], "--static-dir") == 0 && i + 1 < argc) {
            static_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: iris-ha-gw [--port N] [--workers N] [--no-pin]"
                        " [--dataset PATH] [--static-dir DIR]\n");
            return 0;
        }
    }

    iris::http::start_date_clock();
    g_pipeline = build_template("text/plain", kOk);

    const bool ds_ok = iris::ha::load_dataset(dataset);
    const bool st_ok = iris::ha::load_static(static_dir);

    std::printf("[iris-ha-gw] listening on :%u dataset=%s(%zu items) static=%s(%s)\n",
                static_cast<unsigned>(cfg.port), dataset,
                iris::ha::dataset_size(), static_dir, st_ok ? "ok" : "missing");
    if (!ds_ok) {
        std::fprintf(stderr, "[iris-ha-gw] dataset not loaded; /json -> 503\n");
    }
    std::fflush(stdout);

    return iris::net::run_server(cfg, handle);
}
