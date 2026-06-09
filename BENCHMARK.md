# IRIS Benchmark Methodology

> This document explains IRIS's experimental design compared to ajv, ensuring reproducible and auditable conclusions.

## Experiment Subjects

- **IRIS** Fast Path interpreter (no JIT, slow path not enabled)
- **ajv** v8.17.1 ([ebdrup/json-schema-benchmark](https://github.com/ebdrup/json-schema-benchmark) long-term leader)
  - Configuration: `{allErrors: false, strict: false, coerceTypes: false, useDefaults: false}` — ajv documentation's recommended fastest configuration

## Fairness Constraints

| Dimension | Handling |
| --- | --- |
| Schema loading | Both compile once, not included in loop |
| Input parsing | Both include `JSON.parse` / Fused parse. This is real industrial cost |
| Error branches | Both run 100% valid corpus, avoiding error code serialization bias |
| Memory allocation | Both disable verbose errors (ajv allErrors=false, IRIS stops at first return) |
| Measurement caliber | Both use wall-clock `steady_clock` / `process.hrtime.bigint()` |

## Corpus

`bench/gen_corpus.cpp` generates reproducible corpus (fixed seed = `0xC0FFEE`):

- `schema.json`: 4-field person schema (name/age/email/active)
- `data.jsonl`: N valid JSON records (typical 50-90 bytes/line)
- `bad.jsonl`: N/10 records intentionally violating schema (reserved for future negative-path bench)

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

## Reproduction Command

```bash
./scripts/compare.sh 1000000 3
```

## Measured Results (Apple M2 Pro, clang 17, CMake Release + LTO)

```
[iris] 0.177 s | 16.91 Mops/s | 970.25 MiB/s | avg 59.1 ns/op
[ajv]  1.033 s | 2.90  Mops/s | 166.66 MiB/s | avg 344.3 ns/op
IRIS speedup vs ajv: 5.83x
```

## Comparison with Whitepaper Goals

> Whitepaper Section 1: Push JSON validation throughput ceiling to within 5× of pure parsing libraries (simdjson).

- simdjson on Apple M2 parses similar-scale JSON at ~3 GB/s (parse only, no validation)
- IRIS Fast Path: ~0.95 GB/s (**parse + validate**)
- Ratio: simdjson / IRIS ≈ 3.16× ⇒ **Within "5× ceiling" target range** ✅

## Known Limitations (Capabilities Not in This Benchmark)

- **Multi-level nested schema**: Current Fast Path treats nested object/array as "structure OK then pass", no internal field validation. Complex schemas on GitHub leaderboards (like GeoJSON) get routed directly to slow path by Inspector; slow path interpreter not yet wired in, results fall back to Fast Path shallow validation.
- **`pattern`, `$ref`, `allOf`**: Schema document parsing stage explicitly rejects these, requires Phase 4 slow path completion.
- **JIT codegen**: Currently stub; target 7-10× ajv (already landed W^X memory layer + register orchestration expected to add another 50%).

## CI

`.github/workflows/bench.yml` triggers on every push:
- ubuntu-24.04 (x86_64 AVX2)
- macos-14 (Apple Silicon NEON)

Fully runs unit tests + Phase 1 SIMD demo + compare.sh three phases, attaching 1M corpus comparison report.
