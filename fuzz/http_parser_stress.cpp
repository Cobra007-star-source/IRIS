// =============================================================================
// fuzz/http_parser_stress.cpp
//
// Portable robustness driver for iris::http::parse_request, runnable anywhere
// ASan/UBSan are available (the libFuzzer runtime is absent from Apple's
// command-line clang, so this is the local equivalent of the iris_http_fuzz
// target). It hammers the parser with three input families:
//
//   1. uniformly random bytes,
//   2. truncations of valid requests (incremental-parse paths),
//   3. valid requests with random byte flips and pipelined concatenations,
//
// and verifies the parser contract (no crash/OOB; consumed <= len; parsed
// views stay within the input span). Exit code is non-zero if any check fails.
//
// Usage: iris_http_stress [iterations]   (default 5,000,000)
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "iris/http/parser.hpp"

namespace {

const char* kSamples[] = {
    "GET /plaintext HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /json HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    "POST /a HTTP/1.0\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
    "GET / HTTP/1.1\r\n\r\n",
    "HEAD /x?y=1&z=2 HTTP/1.1\r\nHost: h\r\nUser-Agent: wrk\r\nAccept: */*\r\n\r\n",
};
constexpr int kNumSamples = sizeof(kSamples) / sizeof(kSamples[0]);

bool check(const char* buf, std::size_t len) {
    iris::http::Request req;
    auto r = iris::http::parse_request(buf, len, req);
    if (r.status == iris::http::ParseStatus::kOk) {
        if (r.consumed > len) {
            std::fprintf(stderr, "FAIL: consumed %zu > len %zu\n", r.consumed, len);
            return false;
        }
        auto in_span = [&](std::string_view v) {
            return v.empty() || (v.data() >= buf && v.data() + v.size() <= buf + len);
        };
        if (!in_span(req.method) || !in_span(req.path)) {
            std::fprintf(stderr, "FAIL: parsed view escaped input span\n");
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    long iters = (argc > 1) ? std::atol(argv[1]) : 5'000'000L;
    std::mt19937_64 rng(0xC0FFEE);

    std::vector<char> buf;
    buf.reserve(1024);

    for (long i = 0; i < iters; ++i) {
        int mode = static_cast<int>(rng() % 3);
        buf.clear();

        if (mode == 0) {
            std::size_t n = rng() % 512;
            buf.resize(n);
            // Fill 8 bytes per RNG draw (the per-byte path dominated runtime).
            for (std::size_t k = 0; k < n; k += 8) {
                std::uint64_t r = rng();
                std::size_t   m = (n - k < 8) ? (n - k) : 8;
                std::memcpy(buf.data() + k, &r, m);
            }
        } else if (mode == 1) {
            const char* s = kSamples[rng() % kNumSamples];
            std::size_t full = std::strlen(s);
            std::size_t n = full ? (rng() % (full + 1)) : 0;
            buf.assign(s, s + n);
        } else {
            int reps = 1 + static_cast<int>(rng() % 3);
            for (int r = 0; r < reps; ++r) {
                const char* s = kSamples[rng() % kNumSamples];
                buf.insert(buf.end(), s, s + std::strlen(s));
            }
            int flips = static_cast<int>(rng() % 6);
            for (int f = 0; f < flips && !buf.empty(); ++f) {
                buf[rng() % buf.size()] = static_cast<char>(rng() & 0xFF);
            }
        }

        static const bool dbg = std::getenv("STRESS_DEBUG") != nullptr;
        if (dbg) {
            std::fprintf(stderr, "i=%ld mode=%d len=%zu : ", i, mode, buf.size());
            for (char c : buf) std::fprintf(stderr, "%02x", static_cast<unsigned char>(c));
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }

        if (!check(buf.data(), buf.size())) {
            std::fprintf(stderr, "crash/contract violation at iter %ld (mode %d, len %zu)\n",
                         i, mode, buf.size());
            return 1;
        }
    }

    std::printf("[stress] %ld iterations, 0 crashes, 0 contract violations\n", iters);
    return 0;
}
