# IRIS — Irisoul's Resolver for Inline Schema

**A from-scratch, high-performance JSON engine in C++20.** Its core is a *fused
parse-and-validate* pipeline — single-pass, zero-DOM, SIMD-vectorized (ARM NEON /
x86-64 AVX2), with AOT perfect hashing and a bitwise-DFA type checker — backed by
an asmjit JIT and a fully spec-compliant slow-path interpreter. IRIS passes
**100% of the official JSON Schema draft 2020-12 test suite (1295 / 1295)**.

The project is now extending the same SIMD + code-generation + zero-allocation
kernel into a **thread-per-core HTTP gateway** aimed at the
[TechEmpower Framework Benchmarks](https://www.techempower.com/benchmarks/).

> 下一代超高性能 JSON 引擎：**Fused Parse & Validate** 单遍扫描 + SIMD 凌空匹配 + Bitwise DFA + AOT 完美哈希，
> 100% 通过 JSON Schema draft 2020-12 官方测试集；正在把同一套内核延伸成 thread-per-core HTTP 网关冲击 TechEmpower。

## 实测数据 (Apple Silicon M-series, clang 17, -O3 -flto, NEON-128)

100,000 条 JSON 记录 × 3 iters，**两套 schema**（扁平 4 字段 + 嵌套 object + array-of-object）：

| 引擎                              | flat Mops/s | flat MiB/s | nested Mops/s | nested MiB/s |
|----------------------------------|------------:|-----------:|--------------:|-------------:|
| simdjson 3.10 (pure parse)       | 16.16       | 926.88     | 12.54         | 1293.22      |
| **IRIS** Fast Path                | **11.21**   | **643.04** | **5.76**      | **594.04**   |
| **IRIS** Slow Path (forced)       | **1.17**    | **67.31**  | **0.55**      | **56.36**    |
| ajv 8.x (Node.js 20)             | 2.48        | 142.06     | 1.15          | 118.84       |

**加速比**：

- **IRIS Fast vs ajv**: flat **× 4.52**, nested **× 5.01**
- **IRIS Fast vs simdjson**: flat × 0.69, nested × 0.46（含 schema 校验）
- **IRIS Slow vs ajv**: flat × 0.47, nested × 0.48（慢车道用于 Fast Path 接不住的复杂 schema）

底层 Phase 1 NEON byte-count 微核：33 GB/s（vs 标量 6.57 GB/s，加速 5.05×）。

### JSON Schema Test Suite 合规率

跑了官方 [json-schema-org/JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite) `draft2020-12` 全部 1295 条 case：

| 维度 | 现版本（双车道 + URI-aware `$ref`） | 早期（仅 Fast Path） |
|------|:----:|:----:|
| **raw pass rate** | **100.00 %** (1295/1295) | 13.82 % (179/1295) |
| **attempted pass rate** | **100.00 %** (1295/1295) | 96.76 % |
| Pass / Fail / Skipped | 1295 / 0 / 0 | 179 / 6 / 1110 |
| Fast Path 接管 | 178 cases (14 %) | 179 |
| **Slow Path 接管** | **1117 cases (86 %)** | 0 |

慢车道（Slow Path）是 IRIS 真正的"安检员"——完整 JSON Schema 2020-12 递归解释器，
覆盖 allOf / anyOf / oneOf / not / if-then-else / `$ref` / `$defs` / `$anchor` /
`$dynamicAnchor` / `$dynamicRef`（真·递归动态作用域）、pattern (**RE2** 后端，
线性时间 ReDoS-safe，含 `\p{Letter}` 长名预处理)、annotation tracking 的
`unevaluatedProperties` / `unevaluatedItems`，外加 URI-aware `$ref` 解析
（RFC 3986 `$id` base + dot-segment 规范化）、跨文档远端注入、`$vocabulary` 仲裁。
Fast Path 编译失败的 schema 自动落到 Slow Path，**永不假阳性通过**。详见
[PERFORMANCE.md §12](./PERFORMANCE.md#12-慢车道slow-path真实现)。

更详细的方法、热点剖析、寄存器溢出策略与对比图见 **[PERFORMANCE.md](./PERFORMANCE.md)**。
一键复现：`./scripts/compare.sh 100000 3`

---

## 1. 目录结构

```
iris/
├── include/iris/        # 公共头文件（API）
│   ├── common.hpp           # cache 对齐 / 内存分配 / 分支提示
│   ├── simd_ops.hpp         # SIMD 字节原语（含 inline 短路 fast peek）
│   ├── json_reader.hpp      # bootstrap JSON 文档解析器（仅 schema 加载用）
│   ├── token_stream.hpp     # SoA token 容器
│   ├── perfect_hash.hpp     # AOT 完美哈希
│   ├── schema.hpp           # CompiledSchema + FieldSpec DSL + ShortKeyTable
│   ├── parser.hpp           # Fused Parse & Validate
│   ├── jit.hpp              # JIT / W^X 内存（Phase 3 骨架）
│   ├── inspector.hpp        # AST 安检员（Tarjan SCC）
│   ├── slow_path.hpp        # 慢车道接口
│   └── validator.hpp        # 顶层双引擎入口
├── src/                 # 实现
├── examples/            # 可运行 demo
│   ├── hello_iris.cpp       # 端到端 schema 编译 + 校验
│   └── phase1_demo.cpp      # SIMD vs 标量 byte-count 吞吐
├── benchmarks/          # 自包含微基准（hardcoded schema）
├── bench/               # 工业级基准 + 协议适配
│   ├── gen_corpus.cpp       # 1M 记录可复现语料（flat + nested）
│   ├── iris_validate.cpp    # CLI: 加载 schema + 校验 JSONL
│   ├── simdjson_bench.cpp   # simdjson 纯解析 baseline（FetchContent）
│   ├── ajv_bench.mjs        # ajv (Node.js) 对照
│   ├── bowtie_iris.cpp      # Bowtie JSON-RPC harness driver
│   └── package.json
├── scripts/
│   └── compare.sh           # 一键对比：build + gen + run iris + run ajv
├── tests/               # 手写测试框架 + 单元测试
└── .github/workflows/   # CI: Linux x86_64 + macOS Apple Silicon
```

## 2. 构建与运行

依赖：`cmake >= 3.20`，`clang/gcc` 支持 C++20。Apple Silicon 原生 NEON，x86_64 自动启 AVX2。LTO 默认开启。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 端到端 demo（看双引擎路由 + 各类校验错误）
./build/examples/hello_iris

# Phase 1 SIMD 吞吐（vs 标量 baseline）
./build/examples/phase1_demo 256

# 通过真实 schema.json 校验 JSONL（CLI）
./build/bench/gen_corpus .corpus 100000
./build/bench/iris_validate .corpus/flat/schema.json   .corpus/flat/data.jsonl   3
./build/bench/iris_validate .corpus/nested/schema.json .corpus/nested/data.jsonl 3

# 三方一键对比（IRIS / simdjson / ajv，需要 Node.js）
./scripts/compare.sh 1000000 3

# Bowtie 协议 smoke test
printf '%s\n' \
  '{"cmd":"start","version":1}' \
  '{"cmd":"dialect","dialect":"https://json-schema.org/draft/2020-12/schema"}' \
  '{"cmd":"run","seq":1,"case":{"schema":{"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]},"tests":[{"instance":{"x":1},"valid":true},{"instance":{},"valid":false}]}}' \
  '{"cmd":"stop"}' | ./build/bench/bowtie_iris

# JIT 路径（asmjit FetchContent，需要联网）
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DIRIS_ENABLE_JIT=ON
cmake --build build-jit -j
./build-jit/bench/iris_validate .corpus/flat/schema.json .corpus/flat/data.jsonl 3
# → engine=fast-jit

# 单元测试
ctest --test-dir build --output-on-failure
```

## 3. 架构落地映射（白皮书 → 代码）

| 白皮书机制 | 实现位置 | 状态 |
| --- | --- | --- |
| Fused Parse & Validate (Zero-DOM) | `src/parser.cpp` | ✅ 递归 object + array |
| 64B Cache 对齐 + SoA | `include/iris/common.hpp`, `token_stream.hpp` | ✅ |
| Bitwise DFA 类型校验 | `parser.cpp` 中 `TypeMask & allowed` | ✅ |
| AOT 完美哈希 (FNV-1a + fmix64) | `src/perfect_hash.cpp` | ✅ |
| ShortKey SWAR fast lookup | `schema.hpp`, `parser.cpp` | ✅ key ≤ 8B 时跳过哈希 |
| SIMD 凌空扫描 (NEON / AVX2) | `src/simd_ops.cpp` + inline header | ✅ |
| 单 SIMD pass 字符串扫描 | `parser.cpp::scan_string` | ✅ `find_byte_pair` |
| AST Inspector / Tarjan SCC | `src/inspector.cpp` | ✅ 算法完成 |
| JSON Schema 文档解析（递归嵌套） | `src/schema.cpp::compile_schema_from_json` | ✅ 包含 nested `properties` / `items` |
| 递归 nested 校验 | `parser.cpp::validate_object/array` | ✅ |
| 慢车道解释器（完整 2020-12 递归求值 + URI-aware `$ref`） | `src/slow_eval.cpp`, `src/slow_schema.cpp` | ✅ 100% 合规（1295/1295） |
| JIT 动态汇编 + W^X | `src/jit.cpp` | ✅ asmjit FetchContent + trampoline 已跑通 |
| MAP_JIT + pthread_jit_write_protect_np | `src/jit.cpp` | ✅ |
| Bowtie JSON-RPC harness driver | `bench/bowtie_iris.cpp` | ✅ start/dialect/run/stop |
| simdjson 横向 baseline | `bench/simdjson_bench.cpp` | ✅ |

## 4. 性能优化时间线

| 优化 | Mops/s | 提升 |
| --- | --- | --- |
| 初版 Fast Path 解释器 | 6.84 | baseline |
| FNV → FNV+fmix64（修正低位扩散） | 7.11 | +4% |
| 单 SIMD pass `find_byte_pair` | 7.37 | +4% |
| LTO/IPO 启用 | 7.53 | +2% |
| ShortKey SWAR (跳过哈希) | 7.53 | ~0%（被下面盖过） |
| **`skip_ws` inline fast peek** | **16.91** | **+125% ← 决定性突破** |

最大瓶颈是 `skip_json_whitespace` 跨 TU 边界无法 inline，每记录 ~10 次调用各损失 ~10 cycles。把判定 `data[0] > 0x20` 内联到头文件即取消该开销。

## 5. Phase Roadmap

- **Phase 1：硅基物理直觉** — ✅ NEON byte-count 33 GB/s，5.05× 标量
- **Phase 2：AOT 静态校验内核** — ✅ 单层 + 嵌套 object/array 端到端 7-16 Mops/s
- **Phase 3：JIT 动态指令生成** — ✅ asmjit trampoline + Compiler API（虚拟寄存器/溢出）；🟡 schema-specialized codegen 下阶段
- **Phase 4：工业级降级 & Bowtie 合规** — ✅ 双车道路由 / 完整慢车道 / ajv & simdjson 横向 / Bowtie driver；**JSON Schema 2020-12 合规率 100%（1295/1295）**
- **Phase 5：TechEmpower 网关** — 🚧 把 SIMD + 零分配内核延伸成 thread-per-core HTTP 网关（见下方 §6）

## 6. 下一步计划 — TechEmpower Gateway（网络优先）

> 校验内核已达世界级（100% 合规）。注意：**TFB 不测 JSON 校验**——`/json` 测序列化、`/plaintext` 测纯路由、其余测数据库。
> 因此下一步是用 IRIS 的 SIMD/零分配肌肉新建一个 HTTP 网关组件，先攻网络-bound 赛道。

1. **网络核心**：thread-per-core epoll/kqueue 事件循环 + `SO_REUSEPORT` + 绑核 + keep-alive
2. **HTTP/1.1 + pipelining**：`picohttpparser` 解析 + 零分配读缓冲 + `writev` 发包 + 1Hz 全局 `Date`
3. **`/plaintext` + `/json`**：拿下两个网络-bound 赛道，配 Dockerfile + `benchmark_config.json` + `wrk` 压测
4. **裸金属调优**：`perf stat`（IPC / L1-dcache / iTLB）+ flamegraph + perf map（最终成绩以 Linux 物理机为准）
5. **第二滩头（延后）**：异步 Postgres 驱动 → `/db` `/queries` `/updates`；`/fortunes`（DB + SIMD XSS 转义 + 模板）
