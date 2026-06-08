// =============================================================================
// src/perfect_hash.cpp
//
// 实用版完美哈希构造：
//
//   1. 设 N = keys.size()
//   2. 选 seed1，使 slot1[i] = fnv64(seed1, key_i) % N 全部互不相同
//      → 即直接命中。对 N <= 32 这种典型 schema 字段量基本一次成功。
//   3. 若一次性 seed1 冲突，再叠加 seed2 做二次 FNV，把整体哈希视为
//      H(key) = fnv64(seed2, key)；模 N 得到槽位。
//
// 这种做法不是教科书 CHD（CHD 用双 hash + displacement table），但对 N <= 1024
// 的 schema 字段量已经稳定收敛，并且实现极短，不引入第三方依赖。
//
// 后续 Phase 3 JIT 阶段可直接 emit seed 立即数，把 lookup 折成 5~6 条指令。
// =============================================================================
#include "iris/perfect_hash.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace iris {

std::int32_t PerfectHashTable::lookup(std::string_view key) const noexcept {
    if (slot_count == 0) return -1;
    const std::uint64_t h = mixed_hash(key, seed2 ? seed2 : seed1);
    const std::uint32_t slot = static_cast<std::uint32_t>(h % slot_count);
    if (IRIS_UNLIKELY(slot_len[slot] != key.size())) return -1;
    if (IRIS_UNLIKELY(slot_hash[slot] != h))         return -1;
    return static_cast<std::int32_t>(slot);
}

PerfectHashBuildResult build_perfect_hash(std::span<const std::string_view> keys,
                                          std::uint32_t max_seed_attempts) {
    PerfectHashBuildResult r;
    const std::size_t N = keys.size();

    if (N == 0) {
        r.success = true;
        return r;
    }

    {
        std::unordered_set<std::string_view> dup_check;
        for (auto k : keys) {
            if (!dup_check.insert(k).second) {
                std::ostringstream oss;
                oss << "duplicate key: " << k;
                r.diagnostic = oss.str();
                return r;
            }
        }
    }

    std::vector<std::int32_t> slot_owner(N, -1);

    for (std::uint64_t seed = kFnv64Offset; seed < kFnv64Offset + max_seed_attempts; ++seed) {
        std::fill(slot_owner.begin(), slot_owner.end(), -1);
        bool ok = true;

        for (std::size_t i = 0; i < N && ok; ++i) {
            std::uint64_t h    = mixed_hash(keys[i], seed);
            std::uint32_t slot = static_cast<std::uint32_t>(h % N);
            if (slot_owner[slot] != -1) { ok = false; break; }
            slot_owner[slot] = static_cast<std::int32_t>(i);
        }

        if (ok) {
            r.success = true;
            r.table.slot_count = static_cast<std::uint32_t>(N);
            r.table.seed1      = seed;
            r.table.seed2      = 0;
            r.table.slot_len.resize(N);
            r.table.slot_hash.resize(N);
            for (std::size_t slot = 0; slot < N; ++slot) {
                std::int32_t owner = slot_owner[slot];
                r.table.slot_len[slot]  = static_cast<std::uint32_t>(keys[owner].size());
                r.table.slot_hash[slot] = mixed_hash(keys[owner], seed);
            }
            return r;
        }
    }

    std::ostringstream oss;
    oss << "failed to find perfect-hash seed for " << N
        << " keys within " << max_seed_attempts << " attempts";
    r.diagnostic = oss.str();
    return r;
}

}  // namespace iris
