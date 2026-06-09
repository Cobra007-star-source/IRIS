// =============================================================================
// iris/perfect_hash.hpp
//
// AOT perfect hash (Phase 2)
//
// Approach (simplified CHD / FCH):
//
//   1. Choose N keys, all known at schema compile time
//   2. Choose seed s so primary hash h1(s, key) maps keys into buckets where
//      collisions are only in a "solvable" range
//   3. Use a small displacement table disp[buckets] to spread collisions into N slots
//
// This version is an engineering simplification:
//
//   - Instead of classic CHD bucket-sort-greedy, we enumerate seed two-step hashes
//     to find a collision-free layout. Sufficient for N <= 256 schema fields
//   - Keys are not stored; only (length, fnv64) dual fingerprints are kept and
//     verified at lookup time, avoiding exposure of key string memory
//
// Hot-path lookup is constant time with one branch: mul/add/cmp.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iris/common.hpp"

namespace iris {

inline constexpr std::uint64_t kFnv64Offset = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnv64Prime  = 0x100000001b3ULL;

IRIS_FORCE_INLINE std::uint64_t fnv64(const char* s, std::size_t n,
                                      std::uint64_t seed = kFnv64Offset) noexcept {
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint8_t>(s[i]);
        h *= kFnv64Prime;
    }
    return h;
}

IRIS_FORCE_INLINE std::uint64_t fnv64(std::string_view s,
                                      std::uint64_t seed = kFnv64Offset) noexcept {
    return fnv64(s.data(), s.size(), seed);
}

// MurmurHash3 finalizer (fmix64):
//   FNV-1a has weak low-bit diffusion; modulo N collides often for small N.
//   Run FNV output through an avalanche function so any 1-bit input difference
//   spreads across 64 output bits. This is the real "slot fingerprint" for PerfectHash.
IRIS_FORCE_INLINE std::uint64_t fmix64(std::uint64_t k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

IRIS_FORCE_INLINE std::uint64_t mixed_hash(std::string_view s,
                                           std::uint64_t seed) noexcept {
    return fmix64(fnv64(s, seed));
}

// Built lookup table
struct PerfectHashTable {
    // Slot count = key count (no empty slots, dense table)
    std::uint32_t slot_count = 0;
    // Primary seed
    std::uint64_t seed1 = 0;
    // Secondary seed (resolves collision buckets)
    std::uint64_t seed2 = 0;
    // Fingerprints for dedup verification: each slot (length, fnv64)
    std::vector<std::uint32_t> slot_len;
    std::vector<std::uint64_t> slot_hash;

    // Lookup: returns slot in [0, slot_count); -1 if key not in set.
    [[nodiscard]] std::int32_t lookup(std::string_view key) const noexcept;
};

// Compile-time table build (runtime function today; may become constexpr in AOT phase)
//
// Not guaranteed to succeed; returns success=false when keys are too many or collisions severe.
struct PerfectHashBuildResult {
    bool             success = false;
    PerfectHashTable table;
    std::string      diagnostic;
};

PerfectHashBuildResult build_perfect_hash(std::span<const std::string_view> keys,
                                          std::uint32_t max_seed_attempts = 1u << 16);

}  // namespace iris
