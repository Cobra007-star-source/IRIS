// =============================================================================
// benchmarks/validate_bench.cpp
//
// 自包含 micro-bench：测 IRIS Fast Path 校验单条 / 批量 JSON 的吞吐。
//
// 不依赖 Google Benchmark，避免引入 fetchcontent 的网络下载。
// 用 chrono + asm volatile clobber 防止编译器把工作整体优化掉。
// =============================================================================
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "iris/simd_ops.hpp"
#include "iris/validator.hpp"

namespace {

using clock_t_ = std::chrono::steady_clock;

void clobber(const void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace iris;
    const std::size_t iters = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;

    std::array<FieldSpec, 4> fields = {{
        {"name",   kTypeString,   .required = true,  .min_string_len = 1, .max_string_len = 64},
        {"age",    kTypeInteger,  .required = true,  .min_int = 0,        .max_int = 200},
        {"email",  kTypeString,   .required = false, .min_string_len = 3, .max_string_len = 128},
        {"active", kTypeBoolean,  .required = false},
    }};

    auto built = compile_schema(fields, false);
    if (!built.ok) {
        std::fprintf(stderr, "compile_schema failed: %s\n", built.diagnostic.c_str());
        return 1;
    }
    Validator v(std::move(built.schema));

    std::printf("[bench] impl=%s engine=%s iters=%zu\n",
                iris::simd::implementation_name(),
                engine_path_name(v.path()), iters);

    const std::vector<std::string> corpus = {
        R"({"name":"Iris","age":24,"email":"hi@iris.dev","active":true})",
        R"({"name":"M2","age":3,"active":false})",
        R"({"name":"长名字测试","age":100,"email":"x@y.z"})",
        R"({"age":12,"name":"out-of-order","active":true})",
        R"({"name":"  spaces  ","age":42})",
    };

    std::size_t total_bytes = 0;
    int         ok_count    = 0;

    auto t0 = clock_t_::now();
    for (std::size_t i = 0; i < iters; ++i) {
        const auto& s = corpus[i % corpus.size()];
        auto r = v.validate(s);
        ok_count += r.ok();
        total_bytes += s.size();
        clobber(&r);
    }
    double secs = std::chrono::duration<double>(clock_t_::now() - t0).count();

    double mqps = iters / secs / 1.0e6;
    double mbps = total_bytes / secs / (1024.0 * 1024.0);

    std::printf("[bench] %.3f s | %.2f Mops/s | %.2f MiB/s | ok=%d/%zu\n",
                secs, mqps, mbps, ok_count, iters);
    return 0;
}
