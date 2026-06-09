# IRIS — Irisoul's Resolver for Inline Schema

**A from-scratch, high-performance JSON engine in C++20.** Its core is a *fused
parse-and-validate* pipeline — single-pass, zero-DOM, SIMD-vectorized (ARM NEON /
x86-64 AVX2), with AOT perfect hashing and a bitwise-DFA type checker — backed by
an asmjit JIT and a fully spec-compliant slow-path interpreter. IRIS passes
**100% of the official JSON Schema draft 2020-12 test suite (1295 / 1295)**.

The project is now extending the same SIMD + code-generation + zero-allocation
kernel into a **thread-per-core HTTP gateway** aimed at the
[TechEmpower Framework Benchmarks](https://www.techempower.com/benchmarks/).

> Next-generation ultra-high-performance JSON engine: **Fused Parse & Validate** single-pass scanning + SIMD in-flight matching + Bitwise DFA + AOT perfect hashing.
> 100% compliant with JSON Schema draft 2020-12 official test suite; now extending the same kernel into a thread-per-core HTTP gateway targeting TechEmpower.

## Benchmark Results (Apple Silicon M-series, clang 17, -O3 -flto, NEON-128)

100,000 JSON records × 3 iterations with **two schemas** (flat 4-field + nested object + array-of-object):

| Engine                           | flat Mops/s | flat MiB/s | nested Mops/s | nested MiB/s |
|----------------------------------|------------:|-----------:|--------------:|-------------:|
| simdjson 3.10 (pure parse)       | 16.16       | 926.88     | 12.54         | 1293.22      |
| **IRIS** Fast Path                | **11.21**   | **643.04** | **5.76**      | **594.04**   |
| **IRIS** Slow Path (forced)       | **1.17**    | **67.31**  | **0.55**      | **56.36**    |
| ajv 8.x (Node.js 20)             | 2.48        | 142.06     | 1.15          | 118.84       |

**Speedup Ratios**:

- **IRIS Fast vs ajv**: flat **× 4.52**, nested **× 5.01**
- **IRIS Fast vs simdjson**: flat × 0.69, nested × 0.46 (with schema validation)
- **IRIS Slow vs ajv**: flat × 0.47, nested × 0.48 (slow path handles complex schemas that fast path cannot)

Underlying Phase 1 NEON byte-count micro-kernel: 33 GB/s (vs scalar 6.57 GB/s, 5.05× speedup).

### JSON Schema Test Suite Compliance

Ran the official [json-schema-org/JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite) `draft2020-12` full 1295 test cases:

| Metric | Current Version (Dual-Path) | Previous (Fast Path Only) |
|------|:----:|:----:|
| **Raw Pass Rate** | **100.00%** (1295/1295) | 13.82% (179/1295) |
| **Attempted Pass Rate** | **100.00%** (1295/1295) | 96.76% |
| Pass / Fail / Skipped | 1295 / 0 / 0 | 179 / 6 / 1110 |
| Fast Path Handled | 294 cases (23%) | 179 |
| **Slow Path Handled** | **1001 cases (77%)** | 0 |

The slow path is IRIS's true "security inspector" — a complete JSON Schema 2020-12 recursive interpreter
covering allOf / anyOf / oneOf / not / if-then-else / `$ref` / `$defs` / `$anchor` /
`$dynamicRef`, pattern (**RE2** backend, linear-time ReDoS-safe), annotation tracking with
`unevaluatedProperties` / `unevaluatedItems`. Schemas that fail to compile in Fast Path automatically
fall back to Slow Path, **ensuring zero false positives**. See [PERFORMANCE.md §12](./PERFORMANCE.md#12-slow-path-real-implementation) for details.

For more detailed methodology, hotspot analysis, register spilling strategies, and comparison charts, see **[PERFORMANCE.md](./PERFORMANCE.md)**.
One-command reproduction: `./scripts/compare.sh 100000 3`

---

## 1. Directory Structure

```
iris/
├── include/iris/        # Public header files (API)
│   ├── common.hpp           # Cache alignment / memory allocation / branch hints
│   ├── simd_ops.hpp         # SIMD byte primitives (with inline short-circuit fast peek)
│   ├── json_reader.hpp      # Bootstrap JSON document parser (schema loading only)
│   ├── token_stream.hpp     # SoA token container
│   ├── perfect_hash.hpp     # AOT perfect hashing
│   ├── schema.hpp           # CompiledSchema + FieldSpec DSL + ShortKeyTable
│   ├── parser.hpp           # Fused Parse & Validate
│   ├── jit.hpp              # JIT / W^X memory (Phase 3 skeleton)
│   ├── inspector.hpp        # AST inspector (Tarjan SCC)
│   ├── slow_path.hpp        # Slow path interface
│   └── validator.hpp        # Top-level dual-engine entry point
├── src/                 # Implementation
├── examples/            # Runnable demos
│   ├── hello_iris.cpp       # End-to-end schema compilation + validation
│   └── phase1_demo.cpp      # SIMD vs scalar byte-count throughput
├── benchmarks/          # Self-contained micro-benchmarks (hardcoded schema)
├── bench/               # Industrial benchmarks + protocol adapters
│   ├── gen_corpus.cpp       # 1M record reproducible corpus (flat + nested)
│   ├── iris_validate.cpp    # CLI: load schema + validate JSONL
│   ├── simdjson_bench.cpp   # simdjson pure parse baseline (FetchContent)
│   ├── ajv_bench.mjs        # ajv (Node.js) comparison
│   ├── bowtie_iris.cpp      # Bowtie JSON-RPC harness driver
│   └── package.json
├── scripts/
│   └── compare.sh           # One-command comparison: build + gen + run iris + ajv
├── tests/               # Hand-written test framework + unit tests
└── .github/workflows/   # CI: Linux x86_64 + macOS Apple Silicon
```

## 2. Build and Run

**Dependencies**: `cmake >= 3.20`, `clang/gcc` with C++20 support. Apple Silicon native NEON, x86_64 auto-enables AVX2. LTO enabled by default.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# End-to-end demo (see dual-engine routing + various validation errors)
./build/examples/hello_iris

# Phase 1 SIMD throughput (vs scalar baseline)
./build/examples/phase1_demo 256

# Validate JSONL with real schema.json (CLI)
./build/bench/gen_corpus .corpus 100000
./build/bench/iris_validate .corpus/flat/schema.json   .corpus/flat/data.jsonl   3
./build/bench/iris_validate .corpus/nested/schema.json .corpus/nested/data.jsonl 3

# Three-way one-command comparison (IRIS / simdjson / ajv, requires Node.js)
./scripts/compare.sh 1000000 3

# Bowtie protocol smoke test
printf '%s\n' \
  '{"cmd":"start","version":1}' \
  '{"cmd":"dialect","dialect":"https://json-schema.org/draft/2020-12/schema"}' \
  '{"cmd":"run","seq":1,"case":{"schema":{"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]},"tests":[{"instance":{"x":1},"valid":true},{"instance":{},"valid":false}]}}' \
  '{"cmd":"stop"}' | ./build/bench/bowtie_iris

# JIT path (asmjit FetchContent, requires internet)
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DIRIS_ENABLE_JIT=ON
cmake --build build-jit -j
./build-jit/bench/iris_validate .corpus/flat/schema.json .corpus/flat/data.jsonl 3
# → engine=fast-jit

# Unit tests
ctest --test-dir build --output-on-failure
```

## 3. Architecture Implementation Mapping (Whitepaper → Code)

| Whitepaper Mechanism | Implementation Location | Status |
| --- | --- | --- |
| Fused Parse & Validate (Zero-DOM) | `src/parser.cpp` | ✅ Recursive object + array |
| 64B Cache Aligned + SoA | `include/iris/common.hpp`, `token_stream.hpp` | ✅ |
| Bitwise DFA Type Validation | `parser.cpp` `TypeMask & allowed` | ✅ |
| AOT Perfect Hashing (FNV-1a + fmix64) | `src/perfect_hash.cpp` | ✅ |
| ShortKey SWAR Fast Lookup | `schema.hpp`, `parser.cpp` | ✅ Skip hash for key ≤ 8B |
| SIMD In-Flight Scan (NEON / AVX2) | `src/simd_ops.cpp` + inline header | ✅ |
| Single SIMD Pass String Scan | `parser.cpp::scan_string` | ✅ `find_byte_pair` |
| AST Inspector / Tarjan SCC | `src/inspector.cpp` | ✅ Algorithm complete |
| JSON Schema Document Parsing (Recursive Nested) | `src/schema.cpp::compile_schema_from_json` | ✅ Includes nested `properties` / `items` |
| Recursive Nested Validation | `parser.cpp::validate_object/array` | ✅ |
| Slow Path Interpreter | `src/slow_path.cpp` | ✅ Full implementation |
| JIT Dynamic Assembly + W^X | `src/jit.cpp` | ✅ asmjit FetchContent + trampoline working |
| MAP_JIT + pthread_jit_write_protect_np | `src/jit.cpp` | ✅ |
| Bowtie JSON-RPC Harness Driver | `bench/bowtie_iris.cpp` | ✅ start/dialect/run/stop |
| simdjson Horizontal Baseline | `bench/simdjson_bench.cpp` | ✅ |

## 4. Performance Optimization Timeline

| Optimization | Mops/s | Improvement |
| --- | --- | --- |
| Initial Fast Path Interpreter | 6.84 | baseline |
| FNV → FNV+fmix64 (fixed low-bit diffusion) | 7.11 | +4% |
| Single SIMD Pass `find_byte_pair` | 7.37 | +4% |
| LTO/IPO Enabled | 7.53 | +2% |
| ShortKey SWAR (skip hash) | 7.53 | ~0% (overshadowed below) |
| **`skip_ws` Inline Fast Peek** | **16.91** | **+125% ← Decisive Breakthrough** |

The biggest bottleneck was `skip_json_whitespace` not being inlinable across TU boundaries, costing ~10 cycles per ~10 calls per record. Inlining the `data[0] > 0x20` check into the header eliminated this overhead.

## 5. Phase Roadmap

- **Phase 1: Silicon-Based Physical Intuition** — ✅ NEON byte-count 33 GB/s, 5.05× scalar
- **Phase 2: AOT Static Validation Kernel** — ✅ Single-layer + nested object/array end-to-end 7-16 Mops/s
- **Phase 3: JIT Dynamic Instruction Generation** — ✅ asmjit trampoline validated; 🟡 schema-specialized codegen next phase
- **Phase 4: Industrial Fallback & Bowtie Compliance** — ✅ Schema parsing / ajv & simdjson horizontal / Bowtie driver / Slow path complete

## 6. Next Steps

1. **JIT v0.2**: Upgrade trampoline to schema-specialized codegen with immediate value embedding
2. **Bowtie OCI Release**: Package `bowtie_iris` and push to `bowtie-json-schema` repository
3. **HTTP Gateway Integration**: Extend SIMD kernel into thread-per-core HTTP server for TechEmpower benchmarks
4. **Apple Instruments Integration**: Auto-run `xctrace --template 'CPU Counters'` in CI to observe Cache Miss / Branch Misprediction

---

## License

GNU Affero General Public License v3.0 - See LICENSE file for details.

## Contributing

Issues and pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.
