# IRIS Benchmark 方法学

> 本文档说明 IRIS 对比 ajv 的实验设计，确保结论可复现、可审计。

## 实验对象

- **IRIS** Fast Path 解释器（无 JIT，未启用慢车道）
- **ajv** v8.17.1（[ebdrup/json-schema-benchmark](https://github.com/ebdrup/json-schema-benchmark) 长期榜首）
  - 配置：`{allErrors: false, strict: false, coerceTypes: false, useDefaults: false}` —— ajv 文档推荐的最快配置

## 公平性约束

| 维度 | 处理 |
| --- | --- |
| Schema 加载 | 两侧均一次性编译，不计入循环 |
| 输入解析 | 都包含 `JSON.parse` / Fused parse。这是工业现场的真实成本 |
| 错误分支 | 都跑 100% 合法语料，避免错误码序列化偏差 |
| 内存分配 | 都禁用 verbose 错误（ajv allErrors=false，IRIS 一返回即停） |
| 实测口径 | 都用 wall-clock `steady_clock` / `process.hrtime.bigint()` |

## 语料

`bench/gen_corpus.cpp` 生成可复现的语料（固定 seed = `0xC0FFEE`）：

- `schema.json`: 4 字段 person schema（name/age/email/active）
- `data.jsonl`: N 条合法 JSON 记录（typical 50-90 字节 / 行）
- `bad.jsonl`: N/10 条故意违反 schema 的负例（保留给后续 negative-path bench）

## Schema

```json
{
  "type": "object",
  "properties": {
    "name":   {"type": "string",  "minLength": 1, "maxLength": 64},
    "age":    {"type": "integer", "minimum": 0,    "maximum": 200},
    "email":  {"type": "string",  "minLength": 3, "maxLength": 128},
    "active": {"type": "boolean"}
  },
  "required": ["name", "age"],
  "additionalProperties": false
}
```

## 复现命令

```bash
./scripts/compare.sh 1000000 3
```

## 实测（Apple M2 Pro，clang 17，CMake Release + LTO）

```
[iris] 0.177 s | 16.91 Mops/s | 970.25 MiB/s | avg 59.1 ns/op
[ajv]  1.033 s | 2.90  Mops/s | 166.66 MiB/s | avg 344.3 ns/op
IRIS speedup vs ajv: 5.83x
```

## 与白皮书目标对照

> 白皮书第一节：将 JSON 校验的吞吐量极限推至纯解析库（simdjson）的 5 倍以内。

- simdjson 在 Apple M2 上对类似规模 JSON 解析约 ~3 GB/s（仅解析，无校验）
- IRIS Fast Path：~0.95 GB/s（**解析 + 校验**）
- 比值：simdjson / IRIS ≈ 3.16× ⇒ **位于"5 倍以内"目标区间内** ✅

## 已知短板（未参与本榜单的能力）

- **多层嵌套 schema**：当前 Fast Path 把嵌套 object/array 当作"结构 OK 即放行"，不做内部字段校验。GitHub 榜单上的复杂 schema（如 GeoJSON）会被 Inspector 直接路由到慢车道，慢车道暂未接入解释器，结果会回退到 Fast Path 的浅校验。
- **`pattern`、`$ref`、`allOf`**：Schema 文档解析阶段会显式拒绝，需 Phase 4 慢车道补齐。
- **JIT codegen**：当前是 stub，目标 7-10× ajv（已落地的 W^X 内存层 + 寄存器编排预期能再提 50%）。

## CI

`.github/workflows/bench.yml` 在每次 push 触发：
- ubuntu-24.04（x86_64 AVX2）
- macos-14（Apple Silicon NEON）

完整跑 unit tests + Phase 1 SIMD demo + compare.sh 三个阶段，附加 1M 语料对比报告。
