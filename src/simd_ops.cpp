// =============================================================================
// simd_ops.cpp
// SIMD byte primitives for zero-allocation scanning over user buffers
// =============================================================================
#include "iris/simd_ops.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #include <arm_neon.h>
    #define IRIS_HAS_NEON 1
#endif

#if defined(__AVX2__)
    #include <immintrin.h>
    #define IRIS_HAS_AVX2 1
#endif

namespace iris::simd {

// -----------------------------------------------------------------------------
// implementation name
// -----------------------------------------------------------------------------
const char* implementation_name() noexcept {
#if defined(IRIS_HAS_AVX2)
    return "avx2-256";
#elif defined(IRIS_HAS_NEON)
    return "neon-128";
#else
    return "scalar";
#endif
}

// -----------------------------------------------------------------------------
// scalar reference implementation
// -----------------------------------------------------------------------------
std::size_t count_byte_scalar(const std::uint8_t* IRIS_RESTRICT data,
                              std::size_t size,
                              std::uint8_t needle) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < size; ++i) {
        n += (data[i] == needle);  // branchless add; compiler emits cmov
    }
    return n;
}

std::size_t find_byte_scalar(const std::uint8_t* IRIS_RESTRICT data,
                             std::size_t size,
                             std::uint8_t needle) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == needle) return i;
    }
    return size;
}

std::size_t find_byte_pair_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                  std::size_t size,
                                  std::uint8_t a,
                                  std::uint8_t b) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == a || data[i] == b) return i;
    }
    return size;
}

// -----------------------------------------------------------------------------
// NEON 128-bit
// -----------------------------------------------------------------------------
#if defined(IRIS_HAS_NEON)

// Single loop: accumulate 64 bytes per flush into 16x u8 lanes.
// u8 lane max 255; every 255 x 16-byte blocks flush u8 accumulator
// to u32 or overflow.
static std::size_t count_byte_neon(const std::uint8_t* data,
                                   std::size_t size,
                                   std::uint8_t needle) noexcept {
    const uint8x16_t v_needle = vdupq_n_u8(needle);
    std::uint64_t total = 0;
    std::size_t i = 0;

    constexpr std::size_t kBlock = 16;
    constexpr std::size_t kFlush = kBlock * 255;  // prevent u8 accumulator overflow

    while (i + kFlush <= size) {
        uint8x16_t acc = vdupq_n_u8(0);
        for (std::size_t j = 0; j < 255; ++j) {
            uint8x16_t v   = vld1q_u8(data + i + j * kBlock);
            uint8x16_t cmp = vceqq_u8(v, v_needle);   // 0xFF or 0x00
            acc = vsubq_u8(acc, cmp);                  // -(-1) = +1
        }
        total += vaddvq_u8(acc);
        i += kFlush;
    }

    {
        uint8x16_t acc = vdupq_n_u8(0);
        while (i + kBlock <= size) {
            uint8x16_t v   = vld1q_u8(data + i);
            uint8x16_t cmp = vceqq_u8(v, v_needle);
            acc = vsubq_u8(acc, cmp);
            i += kBlock;
        }
        total += vaddvq_u8(acc);
    }

    total += count_byte_scalar(data + i, size - i, needle);
    return static_cast<std::size_t>(total);
}

static std::size_t find_byte_neon(const std::uint8_t* data,
                                  std::size_t size,
                                  std::uint8_t needle) noexcept {
    const uint8x16_t v_needle = vdupq_n_u8(needle);
    std::size_t i = 0;
    constexpr std::size_t kBlock = 16;

    while (i + kBlock <= size) {
        uint8x16_t v   = vld1q_u8(data + i);
        uint8x16_t cmp = vceqq_u8(v, v_needle);
        // fold lanes to 64-bit mask: map u8 0xFF to nibble 0xF
        uint8x8_t  narrowed = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
        std::uint64_t mask  = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
        if (IRIS_UNLIKELY(mask != 0)) {
            return i + (__builtin_ctzll(mask) >> 2);
        }
        i += kBlock;
    }
    std::size_t tail = find_byte_scalar(data + i, size - i, needle);
    return (tail == size - i) ? size : i + tail;
}

static std::size_t find_byte_pair_neon(const std::uint8_t* data,
                                       std::size_t size,
                                       std::uint8_t a,
                                       std::uint8_t b) noexcept {
    const uint8x16_t v_a = vdupq_n_u8(a);
    const uint8x16_t v_b = vdupq_n_u8(b);
    std::size_t i = 0;
    constexpr std::size_t kBlock = 16;

    while (i + kBlock <= size) {
        uint8x16_t v   = vld1q_u8(data + i);
        uint8x16_t cmp = vorrq_u8(vceqq_u8(v, v_a), vceqq_u8(v, v_b));
        uint8x8_t  narrowed = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
        std::uint64_t mask  = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
        if (IRIS_UNLIKELY(mask != 0)) {
            return i + (__builtin_ctzll(mask) >> 2);
        }
        i += kBlock;
    }
    std::size_t tail = find_byte_pair_scalar(data + i, size - i, a, b);
    return (tail == size - i) ? size : i + tail;
}

#endif  // IRIS_HAS_NEON

// -----------------------------------------------------------------------------
// AVX2 256-bit
// -----------------------------------------------------------------------------
#if defined(IRIS_HAS_AVX2)

static std::size_t count_byte_avx2(const std::uint8_t* data,
                                   std::size_t size,
                                   std::uint8_t needle) noexcept {
    const __m256i v_needle = _mm256_set1_epi8(static_cast<char>(needle));
    std::uint64_t total = 0;
    std::size_t i = 0;

    constexpr std::size_t kBlock = 32;
    constexpr std::size_t kFlush = kBlock * 255;

    while (i + kFlush <= size) {
        __m256i acc = _mm256_setzero_si256();
        for (std::size_t j = 0; j < 255; ++j) {
            __m256i v   = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(data + i + j * kBlock));
            __m256i cmp = _mm256_cmpeq_epi8(v, v_needle);
            acc = _mm256_sub_epi8(acc, cmp);
        }
        __m256i sums = _mm256_sad_epu8(acc, _mm256_setzero_si256());
        alignas(32) std::uint64_t lanes[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), sums);
        total += lanes[0] + lanes[1] + lanes[2] + lanes[3];
        i += kFlush;
    }

    __m256i acc = _mm256_setzero_si256();
    while (i + kBlock <= size) {
        __m256i v   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i cmp = _mm256_cmpeq_epi8(v, v_needle);
        acc = _mm256_sub_epi8(acc, cmp);
        i += kBlock;
    }
    __m256i sums = _mm256_sad_epu8(acc, _mm256_setzero_si256());
    alignas(32) std::uint64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), sums);
    total += lanes[0] + lanes[1] + lanes[2] + lanes[3];

    total += count_byte_scalar(data + i, size - i, needle);
    return static_cast<std::size_t>(total);
}

static std::size_t find_byte_avx2(const std::uint8_t* data,
                                  std::size_t size,
                                  std::uint8_t needle) noexcept {
    const __m256i v_needle = _mm256_set1_epi8(static_cast<char>(needle));
    std::size_t i = 0;
    constexpr std::size_t kBlock = 32;

    while (i + kBlock <= size) {
        __m256i v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i cmp  = _mm256_cmpeq_epi8(v, v_needle);
        std::uint32_t mask = _mm256_movemask_epi8(cmp);
        if (IRIS_UNLIKELY(mask != 0)) {
            return i + __builtin_ctz(mask);
        }
        i += kBlock;
    }
    std::size_t tail = find_byte_scalar(data + i, size - i, needle);
    return (tail == size - i) ? size : i + tail;
}

static std::size_t find_byte_pair_avx2(const std::uint8_t* data,
                                       std::size_t size,
                                       std::uint8_t a,
                                       std::uint8_t b) noexcept {
    const __m256i v_a = _mm256_set1_epi8(static_cast<char>(a));
    const __m256i v_b = _mm256_set1_epi8(static_cast<char>(b));
    std::size_t i = 0;
    constexpr std::size_t kBlock = 32;

    while (i + kBlock <= size) {
        __m256i v   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i cmp = _mm256_or_si256(_mm256_cmpeq_epi8(v, v_a),
                                      _mm256_cmpeq_epi8(v, v_b));
        std::uint32_t mask = _mm256_movemask_epi8(cmp);
        if (IRIS_UNLIKELY(mask != 0)) {
            return i + __builtin_ctz(mask);
        }
        i += kBlock;
    }
    std::size_t tail = find_byte_pair_scalar(data + i, size - i, a, b);
    return (tail == size - i) ? size : i + tail;
}

#endif  // IRIS_HAS_AVX2

// -----------------------------------------------------------------------------
// public dispatch
// -----------------------------------------------------------------------------
std::size_t count_byte(const std::uint8_t* IRIS_RESTRICT data,
                       std::size_t size,
                       std::uint8_t needle) noexcept {
    if (size == 0) return 0;
#if defined(IRIS_HAS_AVX2)
    return count_byte_avx2(data, size, needle);
#elif defined(IRIS_HAS_NEON)
    return count_byte_neon(data, size, needle);
#else
    return count_byte_scalar(data, size, needle);
#endif
}

std::size_t find_byte(const std::uint8_t* IRIS_RESTRICT data,
                      std::size_t size,
                      std::uint8_t needle) noexcept {
    if (size == 0) return 0;
#if defined(IRIS_HAS_AVX2)
    return find_byte_avx2(data, size, needle);
#elif defined(IRIS_HAS_NEON)
    return find_byte_neon(data, size, needle);
#else
    return find_byte_scalar(data, size, needle);
#endif
}

std::size_t find_byte_pair(const std::uint8_t* IRIS_RESTRICT data,
                           std::size_t size,
                           std::uint8_t a, std::uint8_t b) noexcept {
    if (size == 0) return 0;
#if defined(IRIS_HAS_AVX2)
    return find_byte_pair_avx2(data, size, a, b);
#elif defined(IRIS_HAS_NEON)
    return find_byte_pair_neon(data, size, a, b);
#else
    return find_byte_pair_scalar(data, size, a, b);
#endif
}

// -----------------------------------------------------------------------------
// JSON whitespace skipping
//
// JSON whitespace: space/tab/LF/CR only
// Bitwise OR-tree compare against four target bytes;
// all match => 0xFF per byte, else 0x00.
// -----------------------------------------------------------------------------
std::size_t skip_json_whitespace_full(const std::uint8_t* data, std::size_t size) noexcept {
    std::size_t i = 0;

#if defined(IRIS_HAS_NEON)
    const uint8x16_t v_sp  = vdupq_n_u8(0x20);
    const uint8x16_t v_tab = vdupq_n_u8(0x09);
    const uint8x16_t v_nl  = vdupq_n_u8(0x0A);
    const uint8x16_t v_cr  = vdupq_n_u8(0x0D);

    while (i + 16 <= size) {
        uint8x16_t v   = vld1q_u8(data + i);
        uint8x16_t is_ws =
            vorrq_u8(vorrq_u8(vceqq_u8(v, v_sp), vceqq_u8(v, v_tab)),
                     vorrq_u8(vceqq_u8(v, v_nl), vceqq_u8(v, v_cr)));
        // invert: non-whitespace becomes 0xFF
        uint8x16_t non_ws = vmvnq_u8(is_ws);
        uint8x8_t  narrowed = vshrn_n_u16(vreinterpretq_u16_u8(non_ws), 4);
        std::uint64_t mask  = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
        if (mask != 0) {
            return i + (__builtin_ctzll(mask) >> 2);
        }
        i += 16;
    }
#endif

    while (i < size) {
        std::uint8_t c = data[i];
        if (c != 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) return i;
        ++i;
    }
    return size;
}

}  // namespace iris::simd
