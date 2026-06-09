// =============================================================================
// tests/test_framework.hpp
//
// 50-line hand-rolled test framework.
// =============================================================================
#pragma once

#include <cstdio>
#include <functional>
#include <vector>

namespace iris::testing {

struct TestState {
    const char* name;
    bool        passed;
};

using TestFn = void (*)(TestState&);

struct TestCase {
    const char* name;
    TestFn      fn;
};

std::vector<TestCase>& registry();
int RegisterCase(const char* name, TestFn fn);

#define IRIS_TEST(NAME)                                                  \
    static void NAME(::iris::testing::TestState& _ts);                   \
    namespace {                                                          \
        const int _reg_##NAME =                                          \
            ::iris::testing::RegisterCase(#NAME, &NAME);                 \
    }                                                                    \
    static void NAME(::iris::testing::TestState& _ts)

#define IRIS_EXPECT(cond)                                                \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "    EXPECT failed: %s  @%s:%d\n",      \
                         #cond, __FILE__, __LINE__);                     \
            _ts.passed = false;                                          \
        }                                                                \
    } while (0)

#define IRIS_EXPECT_EQ(a, b)                                             \
    do {                                                                 \
        auto _aa = (a); auto _bb = (b);                                  \
        if (!(_aa == _bb)) {                                             \
            std::fprintf(stderr, "    EXPECT_EQ failed @%s:%d\n",        \
                         __FILE__, __LINE__);                            \
            _ts.passed = false;                                          \
        }                                                                \
    } while (0)

}  // namespace iris::testing
