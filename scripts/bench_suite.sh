#!/usr/bin/env bash
# Fixed-parameter wrk suite used to A/B each optimization round.
# Usage: scripts/bench_suite.sh [label]
set -u
H=http://127.0.0.1:8080
LABEL=${1:-run}

run() { # route, conns
    local r=$1 c=$2
    local out
    out=$(wrk -t4 -c"$c" -d8s "$H$r" 2>/dev/null)
    local rps lat
    rps=$(echo "$out" | awk '/Requests\/sec/{print $2}')
    lat=$(echo "$out" | awk '/^    Latency/{print $2}')
    printf "%-32s c=%-4s %12s req/s   avg %s\n" "$r" "$c" "$rps" "$lat"
}

echo "=== bench [$LABEL] $(date +%H:%M:%S) ==="
run /db 256
run "/queries?queries=20" 64
run "/updates?queries=20" 64
run /fortunes 64
if curl -sf "$H/cached-queries?count=1" >/dev/null 2>&1; then
    run "/cached-queries?count=20" 64
fi
run /json 64
run /plaintext 64
