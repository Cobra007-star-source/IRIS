// =============================================================================
// tests/test_perfect_hash.cpp
// =============================================================================
#include <array>
#include <string_view>

#include "iris/perfect_hash.hpp"
#include "test_framework.hpp"

IRIS_TEST(ph_empty) {
    auto r = iris::build_perfect_hash({});
    IRIS_EXPECT(r.success);
    IRIS_EXPECT_EQ(r.table.lookup("anything"), -1);
}

IRIS_TEST(ph_small) {
    std::array<std::string_view, 5> keys = {
        "name", "age", "email", "active", "tags"};
    auto r = iris::build_perfect_hash(keys);
    IRIS_EXPECT(r.success);

    for (auto k : keys) {
        IRIS_EXPECT(r.table.lookup(k) >= 0);
    }
    IRIS_EXPECT_EQ(r.table.lookup("unknown"), -1);
    IRIS_EXPECT_EQ(r.table.lookup(""), -1);
    IRIS_EXPECT_EQ(r.table.lookup("nam"), -1);
    IRIS_EXPECT_EQ(r.table.lookup("names"), -1);
}

IRIS_TEST(ph_distinct_slots) {
    std::array<std::string_view, 8> keys = {
        "a", "ab", "abc", "abcd", "x", "y", "z", "longer-key-here"};
    auto r = iris::build_perfect_hash(keys);
    IRIS_EXPECT(r.success);
    std::array<bool, 8> seen{};
    for (auto k : keys) {
        auto s = r.table.lookup(k);
        IRIS_EXPECT(s >= 0 && s < 8);
        if (s >= 0 && s < 8) {
            IRIS_EXPECT(!seen[s]);
            seen[s] = true;
        }
    }
}

IRIS_TEST(ph_rejects_duplicates) {
    std::array<std::string_view, 3> keys = {"x", "y", "x"};
    auto r = iris::build_perfect_hash(keys);
    IRIS_EXPECT(!r.success);
}
