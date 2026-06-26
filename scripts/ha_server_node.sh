#!/usr/bin/env bash
# HTTPArena SERVER node (split-host). Builds iris-ha-gw and optionally runs it
# or an official HttpArena #1 opponent via Docker on port 8080.
#
# Opponents (HttpArena leaderboard @4096, engine tier):
#   zix          — baseline + static #1
#   minima-sync  — pipelined #1
#   zeemo        — json #1
#
# Usage (on the application instance):
#   bash scripts/ha_server_node.sh              # build only, print next steps
#   FRAMEWORK=iris   bash scripts/ha_server_node.sh run
#   FRAMEWORK=zix    bash scripts/ha_server_node.sh run
#   FRAMEWORK=minima-sync bash scripts/ha_server_node.sh run
#   FRAMEWORK=zeemo  bash scripts/ha_server_node.sh run
#
# Pairs with: scripts/ha_client_node.sh on the load-generator instance.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

FRAMEWORK=${FRAMEWORK:-iris}
ACTION=${1:-prepare}
PORT=${PORT:-8080}
WORKERS=${WORKERS:-$(nproc)}
HA_DIR=${HA_DIR:-/home/ec2-user/HttpArena}
IRIS_BIN="$ROOT/build-ec2-ha/gateway/iris-ha-gw"

free_port() {
    local port=$1
    docker stop httparena-bench-opponent 2>/dev/null || true
    docker rm   httparena-bench-opponent 2>/dev/null || true
    pkill -9 -f 'iris-ha-gw' 2>/dev/null || true
    local pids
    pids=$(ss -ltnpH "sport = :$port" 2>/dev/null \
        | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u || true)
    [[ -n "$pids" ]] && kill -9 $pids 2>/dev/null || true
    for _ in $(seq 1 20); do
        ss -ltnH "sport = :$port" 2>/dev/null | grep -q . || return 0
        sleep 0.25
    done
}

install_deps() {
    echo "=== [deps] AL2023 build tools + docker ==="
    sudo dnf install -y gcc gcc-c++ make cmake git python3 docker 2>/dev/null || true
    sudo systemctl enable --now docker 2>/dev/null || true
    sudo usermod -aG docker "$USER" 2>/dev/null || true
}

clone_httparena() {
    if [[ ! -d "$HA_DIR/.git" ]]; then
        echo "=== [clone] HttpArena -> $HA_DIR ==="
        GIT_HTTP_VERSION=1.1 git clone --depth 1 \
            https://github.com/MDA2AV/HttpArena.git "$HA_DIR"
    else
        echo "=== [skip] HttpArena already at $HA_DIR ==="
    fi
}

build_iris() {
    echo "=== [build] iris-ha-gw ==="
    cmake -B build-ec2-ha -DCMAKE_BUILD_TYPE=Release \
        -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing \
        -DIRIS_BUILD_DB=OFF -DIRIS_BUILD_EXAMPLES=OFF \
        -DIRIS_BUILD_BENCHMARKS=OFF -DIRIS_BUILD_TESTS=OFF \
        -DIRIS_BUILD_SIMDJSON=OFF -DIRIS_BUILD_BOWTIE=OFF \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON 2>/dev/null || \
    cmake -B build-ec2-ha -DCMAKE_BUILD_TYPE=Release \
        -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing \
        -DIRIS_BUILD_DB=OFF -DIRIS_BUILD_EXAMPLES=OFF \
        -DIRIS_BUILD_BENCHMARKS=OFF -DIRIS_BUILD_TESTS=OFF \
        -DIRIS_BUILD_SIMDJSON=OFF -DIRIS_BUILD_BOWTIE=OFF
    cmake --build build-ec2-ha --target iris-ha-gw -j"$(nproc)"
}

build_opponent_image() {
    local fw=$1
    echo "=== [docker build] httparena-$fw (first run may take 10-30 min) ==="
    docker build -t "httparena-$fw" "$HA_DIR/frameworks/$fw"
}

run_iris() {
    free_port "$PORT"
    local cores
    cores=$(seq -s, 0 $((WORKERS - 1)))
    taskset -c "$cores" "$IRIS_BIN" --port "$PORT" --workers "$WORKERS" \
        --dataset "$HA_DIR/data/dataset.json" \
        --static-dir "$HA_DIR/data/static" \
        >/tmp/iris-ha.log 2>&1 &
    sleep 2
    head -3 /tmp/iris-ha.log || true
}

run_opponent() {
    local fw=$1
    free_port "$PORT"
    build_opponent_image "$fw"
    docker run -d --name httparena-bench-opponent --network host \
        --security-opt seccomp=unconfined \
        --ulimit memlock=-1:-1 \
        --ulimit nofile=1048576:1048576 \
        -v "$HA_DIR/data/dataset.json:/data/dataset.json:ro" \
        -v "$HA_DIR/data/static:/data/static:ro" \
        -v "$HA_DIR/certs:/certs:ro" \
        "httparena-$fw"
    sleep 3
    docker logs --tail 5 httparena-bench-opponent 2>/dev/null || true
}

case "$ACTION" in
    prepare)
        install_deps
        clone_httparena
        build_iris
        ;;
    run)
        install_deps
        clone_httparena
        [[ -x "$IRIS_BIN" ]] || build_iris
        case "$FRAMEWORK" in
            iris) run_iris ;;
            zix|minima-sync|zeemo) run_opponent "$FRAMEWORK" ;;
            *) echo "unknown FRAMEWORK=$FRAMEWORK (iris|zix|minima-sync|zeemo)" >&2; exit 1 ;;
        esac
        ;;
    stop)
        free_port "$PORT"
        echo "stopped"
        ;;
    *)
        echo "usage: $0 [prepare|run|stop]" >&2
        exit 1
        ;;
esac

PRIV_IP=$(ip -4 -o addr show ens5 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
[[ -z "$PRIV_IP" ]] && PRIV_IP=$(hostname -I | awk '{print $1}')

echo ""
echo "================ HTTPArena SERVER NODE ================"
echo "  private IP : $PRIV_IP"
echo "  port       : $PORT"
echo "  framework  : $FRAMEWORK ($ACTION)"
echo ""
echo "On the LOAD-GENERATOR node:"
echo "  TARGET_HOST=$PRIV_IP bash scripts/ha_client_node.sh"
echo ""
echo "Switch opponent (stop first, then run):"
echo "  bash scripts/ha_server_node.sh stop"
echo "  FRAMEWORK=iris        bash scripts/ha_server_node.sh run"
echo "  FRAMEWORK=zix         bash scripts/ha_server_node.sh run   # baseline/static #1"
echo "  FRAMEWORK=minima-sync bash scripts/ha_server_node.sh run   # pipelined #1"
echo "  FRAMEWORK=zeemo       bash scripts/ha_server_node.sh run   # json #1"
echo ""
echo "Open SG: inbound TCP $PORT from the client node's private IP / SG."
