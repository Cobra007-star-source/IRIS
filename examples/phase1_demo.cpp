// =============================================================================
// examples/phase1_demo.cpp
//
// Phase 1: SIMD vs scalar byte-count throughput on 256 MiB random text.
// Builds physical intuition for the whitepaper.
//
// No Google Benchmark; uses chrono;
// reports GB/s and speedup.
// =============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "iris/simd_ops.hpp"

namespace {

using clock_t_ = std::chrono::steady_clock;

double seconds_since(clock_t_::time_point start) {
    auto end = clock_t_::now();
    return std::chrono::duration<double>(end - start).count();
}

double gbps(std::size_t bytes, double secs) {
    return bytes / secs / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t mib = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 256;
    const std::size_t N   = mib * 1024 * 1024;

    std::vector<std::uint8_t> buf(N);
    std::mt19937_64 rng(0xCAFEBABE);
    std::uniform_int_distribution<int> dist(32, 126);
    for (std::size_t i = 0; i < N; ++i) buf[i] = static_cast<std::uint8_t>(dist(rng));
    for (std::size_t i = 0; i < N; i += 137) buf[i] = '\n';

    const std::uint8_t needle = '\n';
    std::printf("[phase1] impl=%s buffer=%zu MiB needle=0x%02x\n",
                iris::simd::implementation_name(), mib, needle);

    constexpr int kIters = 5;

    std::size_t s_count = 0;
    double s_total = 0;
    for (int it = 0; it < kIters; ++it) {
        auto t0 = clock_t_::now();
        s_count = iris::simd::count_byte_scalar(buf.data(), buf.size(), needle);
        s_total += seconds_since(t0);
    }
    double s_secs = s_total / kIters;

    std::size_t v_count = 0;
    double v_total = 0;
    for (int it = 0; it < kIters; ++it) {
        auto t0 = clock_t_::now();
        v_count = iris::simd::count_byte(buf.data(), buf.size(), needle);
        v_total += seconds_since(t0);
    }
    double v_secs = v_total / kIters;

    std::printf("[phase1] scalar : count=%zu  %.3f ms  %.2f GB/s\n",
                s_count, s_secs * 1000.0, gbps(N, s_secs));
    std::printf("[phase1] simd   : count=%zu  %.3f ms  %.2f GB/s\n",
                v_count, v_secs * 1000.0, gbps(N, v_secs));
    std::printf("[phase1] speedup: %.2fx\n", s_secs / v_secs);
    return (s_count == v_count) ? 0 : 1;
}
