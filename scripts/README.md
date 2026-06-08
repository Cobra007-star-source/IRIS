# IRIS gateway — benchmarking toolkit

Three scripts turn a fresh Linux box into a TechEmpower proving ground and read
the gateway's behavior down to the CPU pipeline. Run them in this order.

## Node roles

A real run uses **two machines on the same LAN** (never load-test over loopback —
it measures the loopback stack, not the server):

| Role            | Runs                          | Purpose                              |
|-----------------|-------------------------------|--------------------------------------|
| **Gateway node**| `iris-gw` + perf scripts      | the system under test                |
| **Load-gen node**| `wrk`                        | saturates the gateway over the NIC   |

`setup_target_env.sh` installs the tools for *both* roles, so run it on each box.

## 1. `setup_target_env.sh` — bootstrap (both nodes)

```bash
scp setup_target_env.sh user@host:~
ssh user@host 'bash setup_target_env.sh'
```
Installs build-essential/cmake/clang + `perf` (gateway) and builds `wrk`
(load-gen), then applies high-concurrency kernel network tuning.

## 2. Build + launch the gateway (gateway node)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing
cmake --build build --target iris-gw -j"$(nproc)"

# pin workers to physical cores 0-3
taskset -c 0-3 ./build/gateway/iris-gw --port 8089 --workers 4
```

## 3. Drive load (load-gen node)

```bash
# plaintext, 16x pipelined (TFB methodology)
wrk -t8 -c128 -d15s --latency -s scripts/pipeline.lua http://<GATEWAY_IP>:8089/plaintext
# json
wrk -t8 -c128 -d15s --latency                         http://<GATEWAY_IP>:8089/json
```
Locally (sanity only), `scripts/loadtest.sh <host> <port>` falls back to `ab`
when `wrk` is absent.

## 4. Telemetry — run WHILE wrk is saturating (gateway node)

```bash
# WHERE time goes -> CPU flamegraph SVG
scripts/generate_flamegraph.sh 15 999

# WHY time goes there -> hardware counters (IPC, L1 misses, branch misses, migrations)
scripts/run_perf_stat.sh 15
CORES=0-3 scripts/run_perf_stat.sh 15     # system-wide across pinned workers
```

Reading: IPC > 2.0 healthy (< 1.0 = pipeline stall); low L1-dcache miss rate;
branch-misses < ~1%; context-switches / cpu-migrations ~0 under load (else the
`taskset` pinning isn't holding).
