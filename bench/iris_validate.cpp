// =============================================================================
// bench/iris_validate.cpp
//
// 标准化 CLI:
//   iris_validate <schema.json> <data.jsonl> [iterations=1]
//
// 行为：
//   1. 加载 schema → CompiledSchema
//   2. mmap / 整文件读 data.jsonl
//   3. 按 '\n' 切片，预存指针（不分配新缓冲）
//   4. 跑 N 轮验证，统计平均吞吐
//
// 故意把 I/O 与解析隔离开（schema 加载、文件读取不计入 bench 时间），
// 这样和 ajv_bench.mjs 才是 apples-to-apples。
// =============================================================================
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "iris/slow_schema.hpp"
#include "iris/validator.hpp"

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

struct Slice { const char* p; std::size_t n; };

std::vector<Slice> split_lines(const std::string& s) {
    std::vector<Slice> out;
    out.reserve(s.size() / 64);
    const char* base = s.data();
    const char* end  = base + s.size();
    const char* line = base;
    for (const char* q = base; q < end; ++q) {
        if (*q == '\n') {
            if (q > line) out.push_back({line, static_cast<std::size_t>(q - line)});
            line = q + 1;
        }
    }
    if (line < end) out.push_back({line, static_cast<std::size_t>(end - line)});
    return out;
}

void clobber(const void* p) { asm volatile("" : : "g"(p) : "memory"); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s [--slow] <schema.json> <data.jsonl> [iterations=1]\n"
                     "  --slow : 强制走 SlowSchema 解释器（绕过 Fast Path 编译路径）\n",
                     argv[0]);
        return 2;
    }
    bool force_slow = false;
    int argi = 1;
    if (std::strcmp(argv[argi], "--slow") == 0) { force_slow = true; ++argi; }
    if (argi + 2 > argc) { std::fprintf(stderr, "missing args\n"); return 2; }

    std::string schema_path = argv[argi];
    std::string data_path   = argv[argi + 1];
    int iterations          = (argc > argi + 2) ? std::atoi(argv[argi + 2]) : 1;
    if (iterations < 1) iterations = 1;

    auto schema_text = slurp(schema_path);
    auto data        = slurp(data_path);
    auto lines       = split_lines(data);

    // 路径 A：Fast Path（默认）—— Validator 构造时自动选路
    // 路径 B：--slow —— 直接用 SlowSchema + validate_slow_path
    std::unique_ptr<iris::SlowSchema> slow_schema;
    iris::Validator vfast(iris::CompiledSchema{});

    using clk = std::chrono::steady_clock;
    const char* engine_label = "?";

    if (force_slow) {
        auto built = iris::compile_slow_schema(schema_text);
        if (!built.ok) {
            std::fprintf(stderr, "slow compile failed: %s\n", built.diagnostic.c_str());
            return 3;
        }
        slow_schema = std::move(built.schema);
        engine_label = "slow-fallback (forced)";
    } else {
        auto built = iris::compile_schema_from_json(schema_text);
        if (!built.ok) {
            std::fprintf(stderr, "fast schema compile failed: %s\n", built.diagnostic.c_str());
            return 3;
        }
        vfast = iris::Validator(std::move(built.schema));
        engine_label = iris::engine_path_name(vfast.path());
    }

    std::printf("[iris] schema=%s lines=%zu engine=%s iters=%d\n",
                schema_path.c_str(), lines.size(), engine_label, iterations);

    auto t0 = clk::now();
    std::int64_t ok_count = 0;
    if (force_slow) {
        for (int it = 0; it < iterations; ++it) {
            for (auto& s : lines) {
                auto r = iris::validate_slow_path(std::string_view(s.p, s.n), *slow_schema);
                ok_count += r.ok();
                clobber(&r);
            }
        }
    } else {
        for (int it = 0; it < iterations; ++it) {
            for (auto& s : lines) {
                auto r = vfast.validate(std::string_view(s.p, s.n));
                ok_count += r.ok();
                clobber(&r);
            }
        }
    }
    double secs = std::chrono::duration<double>(clk::now() - t0).count();
    const std::size_t total_ops   = static_cast<std::size_t>(iterations) * lines.size();
    const std::size_t total_bytes = static_cast<std::size_t>(iterations) * data.size();

    double mops   = total_ops / secs / 1.0e6;
    double mbps   = total_bytes / secs / (1024.0 * 1024.0);
    double avg_ns = secs / total_ops * 1.0e9;

    std::printf("[iris] %.3f s | %.2f Mops/s | %.2f MiB/s | avg %.1f ns/op | ok=%lld/%zu\n",
                secs, mops, mbps, avg_ns,
                static_cast<long long>(ok_count), total_ops);
    return 0;
}
