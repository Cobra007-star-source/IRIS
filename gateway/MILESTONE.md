# IRIS Gateway — Milestone Report (Network-First Beachhead)

Status of the TechEmpower gateway built on top of the IRIS SIMD / zero-allocation
kernel. Cadence follows the "全生命周期铁血质量验证与极限跑分对抗" handbook:
every milestone reports **what shipped**, **how it was verified**, and **what
the next measurement must prove**.

---

## 1. Scope shipped (Phases 0–2 complete, Phase 3 partial)

| Phase | Deliverable | State |
|-------|-------------|:----:|
| P0 | Repo hygiene (untrack 543 generated files), README reposition, CMake `iris-gw` target + `IRIS_PROFILE`, vendor `picohttpparser`, `src/net` + `src/http` + `gateway/` layout | ✅ |
| P1 | Thread-per-core event loop (epoll/kqueue), `SO_REUSEPORT`, `TCP_NODELAY`, pinned workers, keep-alive lifecycle | ✅ |
| P1 | HTTP/1.1 parse (picohttpparser) + pipelining + bounded zero-alloc read buffers | ✅ |
| P1 | Response writer: precomputed per-route templates + lock-free 1 Hz `Date` | ✅ |
| P1 | Quality gate: parser fuzz + thread-safety | ✅ (local UBSan + contract; ASan/TSan/libFuzzer deferred to Linux — see §6) |
| P2 | `/plaintext` + `/json` routes (per-request JSON serialization) | ✅ |
| P2 | **JIT `/json` serializer**: asmjit block-write codegen, dual backend (aarch64 + x86-64), opt-in via `-DIRIS_ENABLE_JIT=ON` | ✅ (see §3) |
| P2 | TFB harness: `iris.dockerfile`, `benchmark_config.json`, `scripts/loadtest.sh`, correctness | ✅ (Docker build verified; Linux native build on AL2023 — see §5) |
| P3 | `perf` counters + flamegraph + perf map, bare-metal report | ⏳ Phase 1 loopback on Linux **done** (§5–§6); NIC-saturated score + PMU on metal — see §7 |

## 2. Architecture (as built)

- **Shared-nothing thread-per-core.** Each worker owns its `Poller`, its own
  `SO_REUSEPORT` listener (kernel load-balances accepts — no accept lock), and a
  free-list pool of `Connection`s with fixed read/write buffers. No mutable state
  is shared between workers.
- **The only cross-thread state is the `Date` string**, published through a
  double buffer + `release/acquire` atomic index (`src/http/date.cpp`). Workers
  never call `time()`/`gmtime()` on the hot path.
- **Zero allocation in steady state.** Connection objects and their buffers are
  allocated once and recycled; parsing, routing, serialization, and flushing all
  operate on preallocated storage (`/json` body uses a 64-byte stack scratch).
- **Precomputed response templates.** Static response bytes (status line, Server,
  Content-Type, Content-Length, and — for `/plaintext` — the body) are built once
  at startup; per request the worker `memcpy`s the template into its own write
  buffer and overwrites just the 29-byte `Date` slot. `/json` re-serializes the
  body each request to honor the TFB rule (via the JIT serializer below, or the
  C++ object serializer as fallback).
- **JIT `/json` body serializer (opt-in).** With `-DIRIS_ENABLE_JIT=ON` the
  `/json` body is emitted by machine code generated at startup (see §3); the body
  is written straight into the connection write buffer with zero intermediate
  copy. Default builds keep the portable C++ object serializer; the hot path is a
  single perfectly-predicted branch on the JIT pointer.
- **Pipelining with backpressure.** A read drains the socket, parses every
  complete request, and batches all responses into one `send()`. Mid-batch write
  blocking compacts unprocessed input and resumes on the next writable event.

Platform split: epoll on Linux (benchmark target), kqueue on macOS (dev). Worker
pinning uses `pthread_setaffinity_np` on Linux; no-op on macOS.

## 3. JIT `/json` serializer — zero-branch block write

With `-DIRIS_ENABLE_JIT=ON` the gateway compiles the `/json` body into native
machine code at startup (asmjit), then executes that code per request to write
the 27 bytes `{"message":"Hello, World!"}` straight into the connection write
buffer. The C++ object serializer runs once at startup as the source of truth
(Content-Length + the exact byte sequence) and remains the runtime fallback when
JIT is unavailable.

**Strategy — wide block write + overlapping tail.** Neither x86-64 nor aarch64
can store an arbitrary 64-bit immediate straight to memory, so the codegen packs
the literal into 64-bit words, materializes each into a scratch GPR (x86
`movabs`, arm64 `movz`/`movk`), and issues one wide store per 8 bytes. The
unaligned tail (`len % 8`) is absorbed by a single **overlapping** 8-byte store
at offset `len-8` rather than a 4/2/1 byte descent — so the whole serializer is
**straight-line, branch-free, loop-free**, the same core idea as a tuned
`memcpy`.

For the 27-byte body this collapses to **4 stores** at offsets `[0, 8, 16, 19]`
(the last overlaps `[16,24)` but lands exactly on byte 26):

- **x86-64 — 10 instructions, 63 bytes:**

```asm
movabs rdx, '{"messag'  ; mov qword ptr [rdi],    rdx
movabs rdx, 'e":"Hell'  ; mov qword ptr [rdi+8],  rdx
movabs rdx, 'o, World'  ; mov qword ptr [rdi+16], rdx
movabs rdx, 'World!"}'  ; mov qword ptr [rdi+19], rdx   ; overlapping tail
mov    rax, 27          ; ret
```

- **aarch64 — 88 bytes:** 4× (`movz` + `movk`×3 to build each 64-bit word) +
  `str`/`stur` (the tail at offset 19 is unaligned → `stur`), then `mov x0, #27`
  + `ret`. The `movz`/`movk` chains are the only "fat" here and are the target of
  the SIMD refinement below.

Verified by `tests/test_jit_serializer.cpp` (13 length cases incl. sub-8, =8, and
overlapping tails, with poison-byte assertions that no store spills past
`length()`). The disassembly above is reproducible from any host via
`examples/jit_serializer_dump.cpp` (asmjit cross-assembles both ISAs; it only
logs, never executes the foreign arch).

**Next level (deferred).** Embed the literal in a rodata island and emit
PC-relative 16-byte SIMD stores (arm64 `ldr q`/`str q`, x86 `movdqu`) — would cut
the 27-byte body to ~2 loads + 2 stores and erase the arm64 `movz`/`movk` chains.
Needs a constant pool + relocation; the GPR block write already removes the
per-byte loop, so this is a refinement, not a blocker.

## 4. Correctness (TFB rules) — PASS

```
/plaintext: PASS 200 text/plain        len=13 body="Hello, World!"
/json:      PASS 200 application/json   len=27 body={"message":"Hello, World!"}
```
Both carry `Server` + `Date` (IMF-fixdate); `Content-Length` matches the bytes.
HTTP/1.1 keep-alive and pipelining verified; HTTP/1.0 keep-alive echoes
`Connection: keep-alive` (fixed an HTTP/1.0 stall found via ApacheBench).

## 5. Functional throughput (loopback sanity, NOT a score)

These numbers prove the hot path under concurrency on real OS stacks. They are
**not** TechEmpower scores (no separate load-gen NIC, no pipeline tuning for TFB).

### macOS dev box (Apple Silicon, 4 workers, `ab -k`, localhost)

| Route        | Requests | Failed | Req/s (loopback) |
|--------------|---------:|-------:|-----------------:|
| `/plaintext` | 256,000  | **0**  | ~200k–215k       |
| `/json`      | 256,000  | **0**  | ~185k–200k       |

### Linux Phase 1 — AWS c6i.4xlarge On-Demand (AL2023, 16 vCPU, epoll + JIT ON)

Gateway: `taskset -c 0-7`, 8 workers, `SO_REUSEPORT`. Load: `wrk`, `taskset -c 8-15`,
128 conns, 6 threads, 15 s, loopback.

| Route        | Req/s      | p50 latency | Notes |
|--------------|-----------:|------------:|-------|
| `/plaintext` | **7.63M**  | 84 µs       | 16× pipelined (`scripts/pipeline.lua`); aggregate HTTP responses/s |
| `/json`      | **807k**   | 78 µs       | JIT block-write serializer; per-request body emission |

Startup log on Linux x86_64: `/json serializer: JIT block-write (27 bytes)`.
First successful run of the **Linux epoll / SO_REUSEPORT** code path outside Docker.

**Real TFB scores require §7** (separate load-gen machine, metal PMU, NIC saturation).

## 6. Quality gate results & environment caveats

- **HTTP parser robustness — PASS (local macOS):** `iris_http_stress` ran **5,000,000**
  random / truncated / mutated / pipelined inputs under **UBSan + explicit
  contract assertions** (`consumed ≤ len`, parsed views stay in-bounds) →
  **0 crashes, 0 contract violations**.
- **Linux Phase 1 — PASS (gateway + unit tests on AL2023):**
  - `/plaintext` + `/json` correctness under `curl` and sustained `wrk` load → **0 failures**.
  - `iris_tests` (incl. JIT serializer cases) → **43/43** on Linux x86_64 after fixing
    a missing `#include <string>` in `perfect_hash.hpp` and GCC-designated-init rules in
    `test_validator.cpp` (Apple Clang accepts mixed designated initializers; GCC 11 does not).
  - `iris_http_stress` (UBSan) → **1,000,000** iterations on AL2023 → **0 crashes, 0 contract violations**.
  - `perf stat` on c6i.4xlarge VM: hardware PMU events `<not supported>` (expected);
    software counters (`cpu-migrations` ≈ 21 under pinned load) confirm worker pinning
    mostly holds. Full IPC/L1 telemetry deferred to `.metal` (§7).
- **Concurrency — PASS (local, functional):** 256k keep-alive requests/route
  across 4 workers on macOS, **0 failures**. The design is shared-nothing per worker;
  see §2.
- **Toolchain limitations on the macOS dev box (Apple Command-Line-Tools, arm64):**
  - `-fsanitize=address` and `-fsanitize=thread` **hang/segfault at process
    startup** (verified with trivial programs — a known Apple CLT runtime issue),
    so ASan and TSan cannot run locally. UBSan works and is used above.
  - The Docker daemon was not running, so the image was not built locally (the
    exact CMake configure/build the image runs is verified natively).
  - `wrk`, `perf`, and `sample` are unavailable/blocked locally.
- **Therefore the following are wired and run on Linux/CI, not here:**
  - `iris_http_fuzz` — libFuzzer harness (auto-skipped where the runtime is
    absent; builds on full clang).
  - TSan build: `cmake -DIRIS_SANITIZER=thread` (LTO auto-disabled).
  - ASan stress: `cmake -DIRIS_STRESS_SANITIZER=address,undefined`.

## 7. Bare-metal Linux measurement plan (Phase 3)

Run on a dedicated Linux host with a separate load-generator NIC.

```bash
# build (racing profile; add -DIRIS_ENABLE_JIT=ON for the JIT /json serializer)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing -DIRIS_ENABLE_JIT=ON
cmake --build build --target iris-gw -j

# low-level counters (targets: IPC > 2.0; low L1-dcache + iTLB miss)
perf stat -e cycles,instructions,L1-dcache-load-misses,iTLB-load-misses,cache-misses \
    ./build/gateway/iris-gw --port 8080
# under load from a second machine:
wrk -t$(nproc) -c512 -d30s --latency http://SERVER:8080/plaintext
wrk -t$(nproc) -c512 -d30s -s scripts/pipeline.lua http://SERVER:8080/plaintext   # 16x pipelined
wrk -t$(nproc) -c512 -d30s --latency http://SERVER:8080/json

# flamegraph
perf record -F 999 -g -p $(pgrep -x iris-gw) -- sleep 30
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg

# steady-state allocation proof
valgrind --tool=massif ./build/gateway/iris-gw --port 8080   # expect flat heap after ramp
```

Acceptance: IPC > 2.0, 0 failed requests, flat heap after connection ramp-up,
correct `Date`/`Server` headers, NIC/CPU saturation on the load generator. With
`-DIRIS_ENABLE_JIT=ON`, confirm the startup log reports
`/json serializer: JIT block-write (27 bytes)` and compare `/json` IPC / branch-
miss counters against the C++-serializer build to quantify the block-write win.

## 8. Deferred — second beachhead

Async pipelined Postgres driver → `/db`, `/queries`, `/updates`,
`/cached-queries`; `/fortunes` (DB fetch + sort + HTML template + SIMD XSS
escape reusing `src/simd_ops.cpp`).
