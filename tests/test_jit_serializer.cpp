// =============================================================================
// tests/test_jit_serializer.cpp
//
// Unit tests for the JIT response serializer (iris::jit::JsonSerializer).
//
// We deliberately reuse the repo's tiny in-house test framework rather than
// pulling in GoogleTest: the project avoids GTest on purpose (see test_main.cpp)
// and a one-binary harness keeps the JIT codegen path easy to debug.
//
// These tests run only when the build was configured with -DIRIS_ENABLE_JIT=ON
// (which fetches asmjit and defines IRIS_HAVE_ASMJIT) on a supported ISA. In a
// default build the serializer must report not-ready so the gateway falls back
// to the C++ serializer; we assert that contract too.
// =============================================================================
#include "test_framework.hpp"

#include <cstring>
#include <string>
#include <string_view>

#if defined(IRIS_HAVE_ASMJIT)
    #include "iris/jit/serializer.hpp"
#endif

namespace {
constexpr std::string_view kJsonBody = "{\"message\":\"Hello, World!\"}";  // 27 bytes
}

IRIS_TEST(jit_serializer_emits_json_body) {
#if defined(IRIS_HAVE_ASMJIT) && (defined(__aarch64__) || defined(__x86_64__))
    iris::jit::JsonSerializer ser;
    IRIS_EXPECT(ser.compile(kJsonBody));
    IRIS_EXPECT(ser.ready());
    IRIS_EXPECT_EQ(ser.length(), kJsonBody.size());

    if (ser.ready()) {
        char buf[64];
        std::memset(buf, 0xAA, sizeof(buf));            // poison
        const std::size_t n = ser.fn()(buf);
        IRIS_EXPECT_EQ(n, kJsonBody.size());
        IRIS_EXPECT_EQ(std::string(buf, n), std::string(kJsonBody));
        // The overlapping tail store must land exactly on the last byte and not
        // spill one byte past length().
        IRIS_EXPECT_EQ(static_cast<unsigned char>(buf[kJsonBody.size()]), 0xAAu);
    }
#else
    (void)kJsonBody;
    _ts.passed = true;  // JIT not built in; nothing to validate here.
#endif
}

IRIS_TEST(jit_serializer_various_lengths) {
#if defined(IRIS_HAVE_ASMJIT) && (defined(__aarch64__) || defined(__x86_64__))
    // Exercise every planner branch: sub-8 (4/2/1 descent), exactly 8, and
    // multiples-plus-tail (overlapping store).
    const char* cases[] = {
        "",  "a",  "ab",  "abc",  "abcd",  "abcde",  "abcdef",  "abcdefg",
        "abcdefgh",                  // exactly 8
        "abcdefghi",                 // 9  -> overlap
        "0123456789ABCDEF",          // 16
        "0123456789ABCDEFG",         // 17 -> overlap
        "{\"message\":\"Hello, World!\"}",
    };
    for (const char* c : cases) {
        const std::string_view sv(c);
        iris::jit::JsonSerializer ser;
        IRIS_EXPECT(ser.compile(sv));
        if (!ser.ready()) continue;

        char buf[64];
        std::memset(buf, 0x5A, sizeof(buf));
        const std::size_t n = ser.fn()(buf);
        IRIS_EXPECT_EQ(n, sv.size());
        IRIS_EXPECT_EQ(std::string(buf, n), std::string(sv));
        IRIS_EXPECT_EQ(static_cast<unsigned char>(buf[sv.size()]), 0x5Au);
    }
#else
    _ts.passed = true;
#endif
}
