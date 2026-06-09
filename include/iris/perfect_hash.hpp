// =============================================================================
// iris/perfect_hash.hpp
//
// AOT 完美哈希 (Phase 2)
//
// 思路（简化版 CHD / FCH）：
//
//   1. 选 N 个 key，全部已知（schema 编译期）
//   2. 选 seed s，使一级哈希 h1(s, key) 将所有 key 桶冲突
//      只发生在“可解决”的范围内
//   3. 用很小的“位移表” disp[buckets] 把冲突再分摊到 N 个槽位
//
// 我们这版做了简单工程化裁剪：
//
//   - 没有走经典 CHD 的 bucket-排序-greedy，而是用“枚举 seed 的两步哈希”
//     找到一个无冲突方案。对 N ≤ 256 这种 schema 字段量完全够用
//   - 不存 key 本身，只存 (length, fnv64) 双重 fingerprint，在查询时验证，
//     避免对外暴露 key 字符串内存
//
// 查询路径在热点上是分支级数 1 的常数时间：mul/add/cmp。
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

// MurmurHash3 finalizer (fmix64)：
//   FNV-1a 的低位扩散较弱，当 slot 数 N 较小时 `% N` 容易碰撞。
//   把 FNV 结果再过一次 avalanche 函数，确保任何 1 bit 输入差异
//   都能平均扩散到 64 bit 输出。这是 PerfectHash 的真实“槽位指纹”。
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

// 构建后的查询表
struct PerfectHashTable {
    // 槽位数 = key 数量（无空槽，密致表）
    std::uint32_t slot_count = 0;
    // 一级 seed
    std::uint64_t seed1 = 0;
    // 二级 seed（用于冲突区间的解决）
    std::uint64_t seed2 = 0;
    // 用于消重对比的指纹：每个槽 (length, fnv64)
    std::vector<std::uint32_t> slot_len;
    std::vector<std::uint64_t> slot_hash;

    // 查询：返回 [0, slot_count) 槽位；若 key 不在集合中返回 -1。
    [[nodiscard]] std::int32_t lookup(std::string_view key) const noexcept;
};

// 编译期表构建（暴露为运行时函数；后续 AOT 阶段可改成 constexpr）
//
// 不能保证总能构造成功；当 keys 过多或冲突严重时返回 success=false。
struct PerfectHashBuildResult {
    bool             success = false;
    PerfectHashTable table;
    std::string      diagnostic;
};

PerfectHashBuildResult build_perfect_hash(std::span<const std::string_view> keys,
                                          std::uint32_t max_seed_attempts = 1u << 16);

}  // namespace iris
