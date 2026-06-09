// =============================================================================
// iris/simd_ops.hpp
//
// SIMD byte-level primitives (Phase 1: hardware-level physical intuition)
//
// Goals:
//   - Zero-copy scan of byte counts / positions on raw buffers
//   - 128-bit NEON on ARM64, 256-bit AVX2 on x86_64
//   - Scalar fallback as correctness oracle
//
// Lowest-level "rock breaker" in the engine. Newline counting, whitespace skip,
// string scan, and quote matching all build on these primitives.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "iris/common.hpp"

namespace iris::simd {

// Name of the implementation used by the current build target (for benchmark reports).
const char* implementation_name() noexcept;

// Count bytes in buffer where byte == needle.
//
// SoA-style pure function: no side effects, zero allocation, stack-only.
// Input may be unaligned; head/tail edge cases handled internally.
[[nodiscard]] std::size_t count_byte(const std::uint8_t* IRIS_RESTRICT data,
                                     std::size_t size,
                                     std::uint8_t needle) noexcept;

[[nodiscard]] IRIS_FORCE_INLINE std::size_t
count_byte(std::string_view view, std::uint8_t needle) noexcept {
    return count_byte(reinterpret_cast<const std::uint8_t*>(view.data()),
                      view.size(), needle);
}

// Scalar reference implementation: callable from benchmarks to measure SIMD speedup.
[[nodiscard]] std::size_t count_byte_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                            std::size_t size,
                                            std::uint8_t needle) noexcept;

// Find first needle byte; returns size on miss.
[[nodiscard]] std::size_t find_byte(const std::uint8_t* IRIS_RESTRICT data,
                                    std::size_t size,
                                    std::uint8_t needle) noexcept;

[[nodiscard]] std::size_t find_byte_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                           std::size_t size,
                                           std::uint8_t needle) noexcept;

// Find first byte equal to a or b; returns size on miss.
// Key accelerator for scan_string: one SIMD pass for '"' and '\\'.
[[nodiscard]] std::size_t find_byte_pair(const std::uint8_t* IRIS_RESTRICT data,
                                         std::size_t size,
                                         std::uint8_t a,
                                         std::uint8_t b) noexcept;

[[nodiscard]] std::size_t find_byte_pair_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                                std::size_t size,
                                                std::uint8_t a,
                                                std::uint8_t b) noexcept;

// Skip leading JSON whitespace (' ', '\t', '\n', '\r'); return offset of first non-whitespace.
//
// Entry point for later in-place tokenization; exported for benchmarks and unit tests.
// Full SIMD implementation for arbitrary-length runs.
[[nodiscard]] std::size_t skip_json_whitespace_full(const std::uint8_t* IRIS_RESTRICT data,
                                                    std::size_t size) noexcept;

// JSON whitespace set {0x20, 0x09, 0x0A, 0x0D} are all <= 0x20.
// Compact JSON is very common: if first byte > 0x20, zero-cost short-circuit.
//
// Intentionally inline in header: scan/validate hot path calls this millions of times;
// cross-TU calls may not fully inline even with LTO.
[[nodiscard]] IRIS_FORCE_INLINE std::size_t
skip_json_whitespace(const std::uint8_t* IRIS_RESTRICT data, std::size_t size) noexcept {
    if (IRIS_LIKELY(size != 0 && data[0] > 0x20)) return 0;
    return skip_json_whitespace_full(data, size);
}

}  // namespace iris::simd
