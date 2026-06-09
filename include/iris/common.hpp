// =============================================================================
// iris/common.hpp
//
// Shared infrastructure:
//   - Cache-line alignment constant (64 bytes)
//   - Compile-time assertions and branch-hint macros (likely/unlikely)
//   - Restricted allocator: DOD requires 64-byte alignment
//   - Non-copyable base class
//
// Whitepaper principle:
//   Data-oriented design (DOD) + 64-byte cache-line alignment to maximize L1 hit rate.
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

// Cache-line alignment attribute
#define IRIS_CACHE_ALIGNED alignas(::iris::kCacheLineBytes)

// Compile-time assertion: require T to fit within N bytes, avoiding "SoA degenerating into AoS".
template <typename T, std::size_t N>
inline constexpr bool size_at_most_v =
    sizeof(T) <= N;

// 64-byte aligned alloc/free (C++17 over-aligned new)
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

// Non-copyable base: state machines/validators typically hold pointers/JIT memory; forbid implicit copy
struct NonCopyable {
    NonCopyable()                              = default;
    NonCopyable(const NonCopyable&)            = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&)                 = default;
    NonCopyable& operator=(NonCopyable&&)      = default;
};

}  // namespace iris
