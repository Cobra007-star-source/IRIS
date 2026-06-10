#!/usr/bin/env bash
# Interleaved A/B benchmark of two iris-gw binaries on the same port.
# Alternating passes (A B A B) cancel out machine-level drift on loopback.
# Usage: scripts/ab_bench.sh <binA> <binB> [passes-per-side]
set -u
BIN_A=$1; BIN_B=$2; PASSES=${3:-2}
DB="host=127.0.0.1 port=5432 user=benchmarkdbuser password=benchmarkdbpass dbname=hello_world"
H=http://127.0.0.1:8080
ROUTES=("/db|256" "/queries?queries=20|64" "/updates?queries=20|64" "/fortunes|64" "/cached-queries?count=20|64" "/json|64")

start() {
    pkill -x iris-gw 2>/dev/null; sleep 0.5
    "$1" --port 8080 --workers 4 --db-pool 8 --db "$DB" >/tmp/ab_gw.log 2>&1 &
    GW_PID=$!
    for i in $(seq 1 50); do curl -sf $H/json >/dev/null 2>&1 && break; sleep 0.1; done
}

bench_pass() { # binary, tag
    start "$1"
    for rc in "${ROUTES[@]}"; do
        r=${rc%|*}; c=${rc#*|}
        if [[ $r == /cached-queries* ]] && ! curl -sf "$H/cached-queries?count=1" >/dev/null 2>&1; then
            continue
        fi
        rps=$(wrk -t4 -c"$c" -d8s "$H$r" 2>/dev/null | awk '/Requests\/sec/{print $2}')
        echo "$2 $r $rps" >>/tmp/ab_results.txt
        printf "  %s %-28s %12s req/s\n" "$2" "$r" "$rps"
    done
    kill $GW_PID 2>/dev/null; wait $GW_PID 2>/dev/null
}

rm -f /tmp/ab_results.txt
for p in $(seq 1 "$PASSES"); do
    echo "--- pass $p ---"
    bench_pass "$BIN_A" A
    bench_pass "$BIN_B" B
done

echo "=== summary (mean of $PASSES passes) ==="
python3 - <<'EOF'
from collections import defaultdict
acc = defaultdict(list)
for line in open("/tmp/ab_results.txt"):
    tag, route, rps = line.split()
    acc[(tag, route)].append(float(rps))
routes = sorted({r for (_, r) in acc})
print(f"{'route':<30} {'A (old)':>12} {'B (new)':>12} {'delta':>8}")
for r in routes:
    a = sum(acc[('A', r)]) / len(acc[('A', r)]) if ('A', r) in acc else None
    b = sum(acc[('B', r)]) / len(acc[('B', r)]) if ('B', r) in acc else None
    if a and b:
        print(f"{r:<30} {a:>12.0f} {b:>12.0f} {100*(b-a)/a:>+7.1f}%")
    elif b:
        print(f"{r:<30} {'-':>12} {b:>12.0f} {'new':>8}")
EOF
