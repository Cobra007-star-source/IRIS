#!/usr/bin/env bash
# =============================================================================
# scripts/loadtest.sh  --  local load test for the IRIS gateway.
#
# Prefers `wrk` (the TechEmpower load tool); falls back to ApacheBench (`ab`).
# Loopback numbers are functional sanity only -- real TFB scores come from a
# bare-metal Linux run (see iris.dockerfile + benchmark_config.json).
#
# Usage:
#   scripts/loadtest.sh [host] [port] [duration] [connections] [threads]
# Defaults: 127.0.0.1 8080 15 256 4
# =============================================================================
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
DUR="${3:-15}"
CONN="${4:-256}"
THREADS="${5:-4}"
BASE="http://${HOST}:${PORT}"

run_wrk() {
  for path in /plaintext /json; do
    echo "=== wrk ${BASE}${path} (c=${CONN}, t=${THREADS}, ${DUR}s) ==="
    wrk -t"${THREADS}" -c"${CONN}" -d"${DUR}s" --latency "${BASE}${path}"
  done
  echo "=== wrk /plaintext pipelined x16 ==="
  wrk -t"${THREADS}" -c"${CONN}" -d"${DUR}s" -s "$(dirname "$0")/pipeline.lua" "${BASE}/plaintext"
}

run_ab() {
  local n=$(( CONN * 4000 ))
  for path in /plaintext /json; do
    echo "=== ab -k ${BASE}${path} (c=${CONN}, n=${n}) ==="
    ab -k -c "${CONN}" -n "${n}" "${BASE}${path}" 2>&1 \
      | grep -E "Requests per second|Failed requests|Keep-Alive|Transfer rate|Time taken" || true
  done
}

echo "[loadtest] target ${BASE}"
if command -v wrk >/dev/null 2>&1; then
  run_wrk
elif command -v ab >/dev/null 2>&1; then
  echo "[loadtest] wrk not found; using ApacheBench (ab) fallback"
  run_ab
else
  echo "[loadtest] neither wrk nor ab found; install one to load test" >&2
  exit 1
fi
