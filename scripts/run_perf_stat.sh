#!/bin/bash
# =============================================================================
# scripts/run_perf_stat.sh
#
# Microarchitecture telemetry for a running iris-gw. Flamegraphs tell you WHERE
# time goes; perf stat tells you WHY -- it pries open the CPU pipeline and reads
# the hardware performance counters that explain stalls.
#
# Run this on the gateway node WHILE a separate machine saturates it with wrk,
# so the counters reflect the hot path under full load.
#
# What we read and why:
#   instructions, cycles       -> IPC (instructions/cycle). Healthy gateway >2.0;
#                                 <1.0 means the pipeline is stalling (branch
#                                 mispredict or memory wait).
#   L1-dcache-load-misses      -> the deadliest signal. High = cache contention,
#                                 e.g. false sharing on the 1 Hz Date double
#                                 buffer / connection-pool cursor, or a
#                                 pipelining buffer straddling a cache line.
#   branch-misses              -> did our hot-loop branch hints actually help?
#                                 (HTTP parse loop, keep-alive/fast-path test).
#   context-switches, migrations, cpu-migrations -> under steady load these must
#                                 be ~0. High = taskset pinning failed, or the
#                                 event loop is busy-waking/sleeping.
#
# Usage:  scripts/run_perf_stat.sh [seconds]        (default 15)
#         CORES=0-3 scripts/run_perf_stat.sh 20      (system-wide on cores 0-3)
# =============================================================================
set -uo pipefail

DURATION="${1:-15}"

PID="$(pgrep -x iris-gw | head -n 1 || true)"
if [ -z "${PID}" ]; then
    echo "error: no running iris-gw process found." >&2
    echo "       start the gateway (ideally: taskset -c 0-3 ./build/gateway/iris-gw --port 8089)" >&2
    echo "       and put it under wrk load, then re-run." >&2
    exit 1
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

# Counter event names vary by CPU vendor / perf version. The canonical generic
# names below work on most x86_64 and modern ARM. We probe each event and keep
# only the supported ones, so a single unknown event never aborts the whole run
# (perf fails hard on an unrecognized -e). Build the -e list dynamically.
CANDIDATES=(
    instructions cycles
    L1-dcache-loads L1-dcache-load-misses
    branch-instructions branch-misses
    context-switches cpu-migrations
    cache-references cache-misses
)

probe_event() {
    # Returns 0 if perf recognizes the event on this host.
    ${SUDO} perf stat -e "$1" true >/dev/null 2>&1
}

EVENTS=()
SKIPPED=()
for ev in "${CANDIDATES[@]}"; do
    if probe_event "${ev}"; then
        EVENTS+=("-e" "${ev}")
    else
        SKIPPED+=("${ev}")
    fi
done

if [ "${#EVENTS[@]}" -eq 0 ]; then
    echo "error: no candidate perf events are supported here (is this a VM with no PMU?)." >&2
    echo "       bare-metal or a PMU-passthrough instance is required for hardware counters." >&2
    exit 2
fi

if [ "${#SKIPPED[@]}" -gt 0 ]; then
    echo "[note] unsupported events skipped on this CPU: ${SKIPPED[*]}"
fi

echo "[perf] probing PID ${PID} for ${DURATION}s."
echo "[perf] >>> START wrk saturation on the load-gen machine NOW <<<"
echo

# --- mode selection ----------------------------------------------------------
# Per-process counting (-p PID) is the default and most targeted. If CORES is
# set (e.g. CORES=0-3), count system-wide on those cores instead -- useful to
# capture migrations/IPC across all worker threads pinned there.
if [ -n "${CORES:-}" ]; then
    echo "[perf] mode: system-wide on cores ${CORES} (IPC/migrations across all pinned workers)"
    ${SUDO} perf stat -C "${CORES}" "${EVENTS[@]}" -- sleep "${DURATION}"
else
    echo "[perf] mode: per-process PID ${PID} (set CORES=0-3 for system-wide)"
    # --per-thread would split by worker; we want the aggregate, so plain -p.
    ${SUDO} perf stat -p "${PID}" "${EVENTS[@]}" -- sleep "${DURATION}"
fi

echo
echo "=================================================="
echo "Reading the result:"
echo "  IPC          = instructions / cycles   (want > 2.0; investigate < 1.0)"
echo "  L1 miss rate = L1-dcache-load-misses / L1-dcache-loads  (want low single-digit %)"
echo "  branch-misses%                          (want < ~1%)"
echo "  context-switches / cpu-migrations       (want ~0 under steady load)"
echo "If IPC is low AND L1 misses are high -> suspect false sharing on shared"
echo "state (Date double buffer, pool cursor) or cache-line-straddling buffers."
echo "=================================================="
