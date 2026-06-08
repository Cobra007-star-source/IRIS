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
| P1 | Quality gate: parser fuzz + thread-safety | ✅ (local UBSan + contract; ASan/TSan/libFuzzer deferred to Linux — see §5) |
| P2 | `/plaintext` + `/json` routes (per-request JSON serialization) | ✅ |
| P2 | TFB harness: `iris.dockerfile`, `benchmark_config.json`, `scripts/loadtest.sh`, correctness | ✅ (image build needs a live Docker daemon / Linux) |
| P3 | `perf` counters + flamegraph + perf map, bare-metal report | ⏳ Linux-only (see §6) |

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
  body each request to honor the TFB rule.
- **Pipelining with backpressure.** A read drains the socket, parses every
  complete request, and batches all responses into one `send()`. Mid-batch write
  blocking compacts unprocessed input and resumes on the next writable event.

Platform split: epoll on Linux (benchmark target), kqueue on macOS (dev). Worker
pinning uses `pthread_setaffinity_np` on Linux; no-op on macOS.

## 3. Correctness (TFB rules) — PASS

```
/plaintext: PASS 200 text/plain        len=13 body="Hello, World!"
/json:      PASS 200 application/json   len=27 body={"message":"Hello, World!"}
```
Both carry `Server` + `Date` (IMF-fixdate); `Content-Length` matches the bytes.
HTTP/1.1 keep-alive and pipelining verified; HTTP/1.0 keep-alive echoes
`Connection: keep-alive` (fixed an HTTP/1.0 stall found via ApacheBench).

## 4. Functional throughput (loopback sanity, NOT a score)

Apple Silicon (M-series), 4 workers, ApacheBench `-k`, localhost:

| Route        | Requests | Failed | Req/s (loopback) |
|--------------|---------:|-------:|-----------------:|
| `/plaintext` | 256,000  | **0**  | ~200k–215k       |
| `/json`      | 256,000  | **0**  | ~185k–200k       |

Loopback + `ab` overhead dominate here; these numbers only prove correctness
under concurrency and the absence of stalls/leaks. **Real scores require §6.**

## 5. Quality gate results & environment caveats

- **HTTP parser robustness — PASS (local):** `iris_http_stress` ran **5,000,000**
  random / truncated / mutated / pipelined inputs under **UBSan + explicit
  contract assertions** (`consumed ≤ len`, parsed views stay in-bounds) →
  **0 crashes, 0 contract violations**.
- **Concurrency — PASS (local, functional):** 256k keep-alive requests/route
  across 4 workers, **0 failures**. The design is shared-nothing per worker;
  see §2.
- **Toolchain limitations on this dev box (Apple Command-Line-Tools, arm64):**
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

## 6. Bare-metal Linux measurement plan (Phase 3)

Run on a dedicated Linux host with a separate load-generator NIC.

```bash
# build (racing profile)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing
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
correct `Date`/`Server` headers, NIC/CPU saturation on the load generator.

## 7. Deferred — second beachhead

Async pipelined Postgres driver → `/db`, `/queries`, `/updates`,
`/cached-queries`; `/fortunes` (DB fetch + sort + HTML template + SIMD XSS
escape reusing `src/simd_ops.cpp`).
