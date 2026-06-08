#!/bin/bash
# =============================================================================
# scripts/generate_flamegraph.sh
#
# One-shot CPU flamegraph for a running iris-gw, using perf + Brendan Gregg's
# FlameGraph toolchain. Run this on the gateway node WHILE the load generator
# (wrk) is hammering it, so the sample captures the hot path under load.
#
# Output: iris_gateway_flamegraph.svg  (scp back to a laptop and open in a
# browser to inspect per-frame cost down to the assembly branch).
#
# Usage:  scripts/generate_flamegraph.sh [seconds] [freq_hz]   (default 15 999)
# =============================================================================
set -euo pipefail

DURATION="${1:-15}"
FREQ="${2:-999}"

# 1. Find the running gateway.
PID="$(pgrep -x iris-gw | head -n 1 || true)"
if [ -z "${PID}" ]; then
    echo "error: no running iris-gw process found. Start the gateway and put it" >&2
    echo "       under load (wrk) first, then re-run this script." >&2
    exit 1
fi

# perf usually needs either root or a relaxed perf_event_paranoid. Pick a sudo
# prefix only if we are not already root.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

echo "[1/4] Sampling PID ${PID} for ${DURATION}s at ${FREQ}Hz (call graph)..."
# -g = capture call stacks. iris-gw is a static C++ binary with frame pointers
# (-fno-omit-frame-pointer in arch_flags.cmake), so DWARF unwinding is not
# required and fp stacks are cheap and accurate.
${SUDO} perf record -F "${FREQ}" -p "${PID}" -g -o perf.data -- sleep "${DURATION}"

echo "[2/4] Exporting samples to text..."
# Make the recording readable without sudo downstream.
${SUDO} chown "$(id -u):$(id -g)" perf.data 2>/dev/null || true
perf script -i perf.data > out.perf

echo "[3/4] Fetching FlameGraph toolchain..."
if [ ! -d "FlameGraph" ]; then
    git clone --depth 1 https://github.com/brendangregg/FlameGraph.git
fi

echo "[4/4] Folding stacks and rendering SVG..."
./FlameGraph/stackcollapse-perf.pl out.perf > out.folded
./FlameGraph/flamegraph.pl \
    --title "IRIS gateway CPU flamegraph (${DURATION}s @ ${FREQ}Hz)" \
    out.folded > iris_gateway_flamegraph.svg

rm -f out.perf out.folded
echo "=================================================="
echo "Done: iris_gateway_flamegraph.svg"
echo "scp it to your laptop and open in a browser to inspect every branch's cost."
echo "Tip: keep wrk running during the sample window for a representative profile."
echo "=================================================="
