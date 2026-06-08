// =============================================================================
// iris/common.hpp
//
// 公共基础设施：
//   - 缓存行对齐常量 (64 字节)
//   - 编译期断言与分支提示宏 (likely/unlikely)
//   - 受限制的内存分配器：DOD 强制要求 64 字节对齐
//   - 不可拷贝的基类
//
// 白皮书原则：
//   面向数据 (DOD) + 64 字节 Cache Line 对齐，让 L1 命中率逼近物理极限。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace iris {

inline constexpr std::size_t kCacheLineBytes = 64;
inline constexpr std::size_t kSimdLaneBytes  = 16;  // NEON / SSE
inline constexpr std::size_t kAvx2LaneBytes  = 32;  // AVX2

#if defined(__GNUC__) || defined(__clang__)
    #define IRIS_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define IRIS_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define IRIS_FORCE_INLINE inline __attribute__((always_inline))
    #define IRIS_NO_INLINE    __attribute__((noinline))
    #define IRIS_RESTRICT     __restrict__
    #define IRIS_PURE         __attribute__((pure))
#else
    #define IRIS_LIKELY(x)   (x)
    #define IRIS_UNLIKELY(x) (x)
    #define IRIS_FORCE_INLINE inline
    #define IRIS_NO_INLINE
    #define IRIS_RESTRICT
    #define IRIS_PURE
#endif

// 缓存行对齐属性
#define IRIS_CACHE_ALIGNED alignas(::iris::kCacheLineBytes)

// 静态期间编译断言：要求 T 被打包至 N 字节内，避免“SoA 退化为 AoS”。
template <typename T, std::size_t N>
inline constexpr bool size_at_most_v =
    sizeof(T) <= N;

// 64 字节对齐分配 / 释放（基于 C++17 over-aligned new）
template <typename T>
[[nodiscard]] inline T* aligned_alloc_n(std::size_t count) {
    void* p = ::operator new(sizeof(T) * count,
                             std::align_val_t{kCacheLineBytes});
    return static_cast<T*>(p);
}

template <typename T>
inline void aligned_free(T* p) noexcept {
    ::operator delete(p, std::align_val_t{kCacheLineBytes});
}

// 不可拷贝基类：状态机/校验器普遍持有指针/JIT 内存，禁止隐式拷贝
struct NonCopyable {
    NonCopyable()                              = default;
    NonCopyable(const NonCopyable&)            = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&)                 = default;
    NonCopyable& operator=(NonCopyable&&)      = default;
};

}  // namespace iris
