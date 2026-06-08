// =============================================================================
// tests/test_main.cpp
//
// 极简手写测试框架。
//
// 选择不引入 GoogleTest / Catch2 是因为：
//   - 减少首次构建的依赖与网络
//   - 测试规模目前 < 50 个 case
//   - 任何 CI 失败的复盘成本都是最低的
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
