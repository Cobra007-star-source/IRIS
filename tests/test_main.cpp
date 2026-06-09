// =============================================================================
// tests/test_main.cpp
//
// Minimal hand-rolled test framework.
//
// We avoid GoogleTest / Catch2 because:
//   - Reduces first-build dependencies and network fetches
//   - Test count is currently < 50 cases
//   - Lowest possible CI failure triage cost
// =============================================================================
#include <cstdio>
#include <vector>

#include "test_framework.hpp"

namespace iris::testing {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> reg;
    return reg;
}

int RegisterCase(const char* name, TestFn fn) {
    registry().push_back({name, fn});
    return 0;
}

}  // namespace iris::testing

int main() {
    int passed = 0;
    int failed = 0;
    for (auto& c : iris::testing::registry()) {
        iris::testing::TestState s{c.name, true};
        std::printf("[run] %s\n", c.name);
        c.fn(s);
        if (s.passed) { ++passed; std::printf("  [ok]\n"); }
        else          { ++failed; std::printf("  [FAIL]\n"); }
    }
    std::printf("\n=== %d passed, %d failed (%zu total) ===\n",
                passed, failed, iris::testing::registry().size());
    return failed == 0 ? 0 : 1;
}
