// =============================================================================
// tests/test_simd.cpp
// =============================================================================
#include <cstring>
#include <random>
#include <string>

#include "iris/simd_ops.hpp"
#include "test_framework.hpp"

IRIS_TEST(simd_count_empty) {
    IRIS_EXPECT_EQ(iris::simd::count_byte(nullptr, 0, 'a'), 0u);
}

IRIS_TEST(simd_count_basic) {
    std::string s = "the quick brown fox jumps over the lazy dog";
    IRIS_EXPECT_EQ(iris::simd::count_byte(s, ' '), 8u);
    IRIS_EXPECT_EQ(iris::simd::count_byte(s, 'z'), 1u);
    IRIS_EXPECT_EQ(iris::simd::count_byte(s, 'Q'), 0u);
}

IRIS_TEST(simd_count_random_vs_scalar) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    for (std::size_t n : {0u, 1u, 7u, 15u, 16u, 17u, 31u, 32u, 33u, 1024u, 4097u, 65537u}) {
        std::string buf(n, '\0');
        for (auto& c : buf) c = static_cast<char>(dist(rng));
        std::uint8_t needle = static_cast<std::uint8_t>(dist(rng));
        auto a = iris::simd::count_byte_scalar(
                reinterpret_cast<const std::uint8_t*>(buf.data()), buf.size(), needle);
        auto b = iris::simd::count_byte(
                reinterpret_cast<const std::uint8_t*>(buf.data()), buf.size(), needle);
        IRIS_EXPECT_EQ(a, b);
    }
}

IRIS_TEST(simd_find_basic) {
    std::string s = "abcdef";
    IRIS_EXPECT_EQ(iris::simd::find_byte(reinterpret_cast<const std::uint8_t*>(s.data()),
                                         s.size(), 'd'), 3u);
    IRIS_EXPECT_EQ(iris::simd::find_byte(reinterpret_cast<const std::uint8_t*>(s.data()),
                                         s.size(), 'z'), 6u);
}

IRIS_TEST(simd_skip_ws) {
    std::string s = "  \t\r\n  X";
    IRIS_EXPECT_EQ(iris::simd::skip_json_whitespace(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size()), 7u);

    std::string s2 = "X";
    IRIS_EXPECT_EQ(iris::simd::skip_json_whitespace(
        reinterpret_cast<const std::uint8_t*>(s2.data()), s2.size()), 0u);

    std::string s3(40, ' ');
    s3 += 'Z';
    IRIS_EXPECT_EQ(iris::simd::skip_json_whitespace(
        reinterpret_cast<const std::uint8_t*>(s3.data()), s3.size()), 40u);
}
