#!/usr/bin/env bash
# HTTPArena CLIENT node (split-host). Drives gcannon/wrk against a remote
# server using the same templates as scripts/benchmark.sh (5s x best-of-3).
#
# Usage:
#   TARGET_HOST=<server private IP> bash scripts/ha_client_node.sh
#   TARGET_HOST=10.0.1.5 PROFILES=baseline,pipelined bash scripts/ha_client_node.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TARGET_HOST=${TARGET_HOST:?set TARGET_HOST to the server private IP}
PORT=${PORT:-8080}
THREADS=${THREADS:-$(nproc)}
CONNS=${CONNS:-4096}
DURATION=${DURATION:-5s}
ROUNDS=${ROUNDS:-3}
WARMUP=${WARMUP:-3s}
PROFILES=${PROFILES:-baseline,pipelined,json,static}
HA_DIR=${HA_DIR:-/home/ec2-user/HttpArena}
REQ="$HA_DIR/requests"

reachable() {
    timeout 5 bash -c "echo >/dev/tcp/$TARGET_HOST/$PORT" 2>/dev/null
}

install_deps() {
    echo "=== [deps] gcannon + wrk ==="
    sudo dnf install -y gcc gcc-c++ make git openssl-devel python3 2>/dev/null || true
    if [[ ! -d "$HA_DIR/.git" ]]; then
        GIT_HTTP_VERSION=1.1 git clone --depth 1 \
            https://github.com/MDA2AV/HttpArena.git "$HA_DIR"
    fi
    if ! command -v gcannon >/dev/null; then
        tmp=$(mktemp -d)
        GIT_HTTP_VERSION=1.1 git clone --depth 1 https://github.com/MDA2AV/gcannon.git "$tmp/gcannon"
        make -C "$tmp/gcannon" -j"$(nproc)"
        sudo cp "$tmp/gcannon/gcannon" /usr/local/bin/
        rm -rf "$tmp"
    fi
    if ! command -v wrk >/dev/null; then
        tmp=$(mktemp -d)
        git clone --depth 1 https://github.com/wg/wrk.git "$tmp/wrk"
        make -C "$tmp/wrk" WITH_OPENSSL=/usr -j"$(nproc)"
        sudo cp "$tmp/wrk/wrk" /usr/local/bin/
        rm -rf "$tmp"
    fi
}

gcannon_rps() {
    # Parse gcannon summary: 2xx count / duration seconds.
    local out=$1
    local dur ok
    dur=$(echo "$out" | grep -oP '(?:requests|frames sent) in \K[\d.]+' | head -1)
    dur=${dur:-1}
    ok=$(echo "$out" | grep -oP '2xx=\K\d+' | head -1)
    ok=${ok:-0}
    awk -v ok="$ok" -v dur="$dur" 'BEGIN { if (dur+0>0) printf "%.0f", ok/dur; else print 0 }'
}

wrk_rps() {
    local out=$1
    echo "$out" | grep -oP 'Requests/sec:\s+\K[\d.]+' | head -1 | cut -d. -f1
}

best_gcannon() {
    local -a args=("$@")
    local best=0 out r
    if [[ "$WARMUP" != "0" && "$WARMUP" != "0s" ]]; then
        gcannon "${args[@]}" >/dev/null 2>&1 || true
    fi
    for _ in $(seq 1 "$ROUNDS"); do
        out=$(gcannon "${args[@]}" 2>&1 || true)
        r=$(gcannon_rps "$out")
        [[ -z "$r" ]] && r=0
        if awk -v a="$r" -v b="$best" 'BEGIN{exit !(a>b)}'; then best=$r; fi
    done
    echo "$best"
}

best_wrk() {
    local -a args=("$@")
    local best=0 out r
    if [[ "$WARMUP" != "0" && "$WARMUP" != "0s" ]]; then
        wrk "${args[@]}" >/dev/null 2>&1 || true
    fi
    for _ in $(seq 1 "$ROUNDS"); do
        out=$(wrk "${args[@]}" 2>&1 || true)
        r=$(wrk_rps "$out")
        [[ -z "$r" ]] && r=0
        if awk -v a="$r" -v b="$best" 'BEGIN{exit !(a>b)}'; then best=$r; fi
    done
    echo "$best"
}

bench_baseline() {
    local url="http://$TARGET_HOST:$PORT"
    local raw="$REQ/get.raw,$REQ/post_cl.raw,$REQ/post_chunked.raw"
    best_gcannon "$url" --raw "$raw" -c "$CONNS" -t "$THREADS" \
        -d "$DURATION" -p 1
}

bench_pipelined() {
    best_gcannon "http://$TARGET_HOST:$PORT/pipeline" \
        -c "$CONNS" -t "$THREADS" -d "$DURATION" -p 16
}

bench_json() {
    local url="http://$TARGET_HOST:$PORT"
    local raw="$REQ/json-1.raw,$REQ/json-5.raw,$REQ/json-10.raw,$REQ/json-15.raw,$REQ/json-25.raw,$REQ/json-40.raw,$REQ/json-50.raw"
    best_gcannon "$url" --raw "$raw" -c "$CONNS" -t "$THREADS" \
        -d "$DURATION" -p 1 -r 25
}

bench_static() {
    best_wrk -t "$THREADS" -c "$CONNS" -d "$DURATION" \
        -s "$REQ/static-rotate.lua" "http://$TARGET_HOST:$PORT"
}

install_deps

if ! reachable; then
    echo "ERROR: cannot reach $TARGET_HOST:$PORT — check SG + server FRAMEWORK=... run" >&2
    exit 1
fi

echo "=== HttpArena-style bench vs $TARGET_HOST:$PORT ==="
echo "threads=$THREADS conns=$CONNS duration=$DURATION best-of-$ROUNDS"
echo ""

printf "%-12s %12s  (official #1 @4096 ref)\n" "PROFILE" "RPS"
printf "%-12s %12s  %s\n" "--------" "------------" "---------------------------"

IFS=',' read -ra PL <<< "$PROFILES"
for p in "${PL[@]}"; do
    case "$p" in
        baseline)
            r=$(bench_baseline)
            ref="zix 4.49M"
            ;;
        pipelined)
            r=$(bench_pipelined)
            ref="minima-sync 57.1M"
            ;;
        json)
            r=$(bench_json)
            ref="zeemo 2.39M"
            ;;
        static)
            r=$(bench_static)
            ref="zix 2.04M"
            ;;
        *)
            echo "skip unknown profile: $p" >&2
            continue
            ;;
    esac
    printf "%-12s %'12.0f  %s\n" "$p" "$r" "$ref"
done

echo ""
echo "Run each opponent on the server, then re-run this script to compare:"
echo "  FRAMEWORK=zix         bash scripts/ha_server_node.sh run"
echo "  FRAMEWORK=minima-sync bash scripts/ha_server_node.sh run"
echo "  FRAMEWORK=zeemo       bash scripts/ha_server_node.sh run"
