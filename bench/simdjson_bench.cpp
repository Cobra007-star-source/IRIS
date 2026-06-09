// =============================================================================
// bench/simdjson_bench.cpp
//
// simdjson baseline: parse-only throughput on same corpus.
//
// simdjson parses only (on-demand), no schema validation;
// interpret as:
//   - IRIS fast path validate = simdjson parse + 1× (ideal bound)
//   - today IRIS < simdjson (extra validation work)
//   - simdjson/IRIS ratio reflects cost of fused parse+validate
//
// Usage:
//   simdjson_bench <data.jsonl> [iterations=1]
// =============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "simdjson.h"

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <data.jsonl> [iters=1]\n", argv[0]);
        return 2;
    }
    std::string data = slurp(argv[1]);
    int iters = (argc > 2) ? std::atoi(argv[2]) : 1;
    if (iters < 1) iters = 1;

    // line split preprocessing (aligned with iris_validate / ajv)
    std::vector<std::string_view> lines;
    {
        const char* p = data.data();
        const char* end = p + data.size();
        const char* line = p;
        for (const char* q = p; q < end; ++q) {
            if (*q == '\n') {
                if (q > line) lines.emplace_back(line, static_cast<std::size_t>(q - line));
                line = q + 1;
            }
        }
    }
    std::printf("[simdjson] impl=%s lines=%zu iters=%d\n",
                simdjson::get_active_implementation()->name().c_str(),
                lines.size(), iters);

    simdjson::ondemand::parser parser;
    std::int64_t ok_count = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int it = 0; it < iters; ++it) {
        for (auto sv : lines) {
            // simdjson requires padded_string or padded_string_view.
            simdjson::padded_string padded(sv.data(), sv.size());
            auto doc = parser.iterate(padded);
            if (doc.error()) continue;
            // trigger parse: walk top-level object keys
            for ([[maybe_unused]] auto field : doc.get_object()) {}
            ++ok_count;
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::size_t total = static_cast<std::size_t>(iters) * lines.size();
    std::size_t bytes = static_cast<std::size_t>(iters) * data.size();
    double mops = total / secs / 1.0e6;
    double mbps = bytes / secs / (1024.0 * 1024.0);
    double avg_ns = secs / total * 1.0e9;
    std::printf("[simdjson] %.3f s | %.2f Mops/s | %.2f MiB/s | avg %.1f ns/op | ok=%lld/%zu\n",
                secs, mops, mbps, avg_ns,
                static_cast<long long>(ok_count), total);
    return 0;
}
