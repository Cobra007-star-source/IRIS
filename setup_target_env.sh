#!/bin/bash
# =============================================================================
# setup_target_env.sh
#
# One-shot bare-metal / enhanced-instance environment bootstrap for IRIS gateway
# benchmarking on Linux. SSH this to a fresh Ubuntu host and run once; it turns
# the box into a "performance proving ground" in a few minutes.
#
# Roles:
#   * Gateway node    : build tools + perf (build & run iris-gw, profile it).
#   * Load-gen node   : wrk (saturate the gateway from a SEPARATE machine).
# Installing both on each host is harmless; use whichever role per machine.
#
# Usage:  scp setup_target_env.sh user@host:~ && ssh user@host 'bash setup_target_env.sh'
# =============================================================================
set -euo pipefail

echo "[1/4] Updating apt and installing base build tools + perf..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    clang \
    git \
    curl \
    linux-tools-common \
    linux-tools-generic \
    "linux-tools-$(uname -r)" \
    htop \
    || echo "[warn] some linux-tools packages may be unavailable on this kernel (common on cloud/virt kernels); perf may need a kernel-matched package."

echo "[2/4] Building and installing wrk (load-generator node)..."
if ! command -v wrk >/dev/null 2>&1; then
    tmpdir="$(mktemp -d)"
    git clone --depth 1 https://github.com/wg/wrk.git "$tmpdir/wrk"
    make -C "$tmpdir/wrk" -j"$(nproc)"
    sudo cp "$tmpdir/wrk/wrk" /usr/local/bin/
    rm -rf "$tmpdir"
else
    echo "  wrk already present: $(command -v wrk)"
fi

echo "[3/4] Tuning kernel network stack for high concurrency..."
# Best-effort: these are runtime (non-persistent) tweaks; ignore failures inside
# unprivileged containers.
sudo sysctl -w net.core.somaxconn=65535            || true
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535  || true
sudo sysctl -w net.core.rmem_max=16777216          || true
sudo sysctl -w net.core.wmem_max=16777216          || true
# Widen the ephemeral port range so the load generator can open many conns.
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535" || true

echo "[4/4] Verifying perf availability..."
perf --version || echo "[warn] perf not runnable; install the kernel-matched linux-tools package for accurate counters."

cat <<'EOF'
==================================================
IRIS proving ground is configured.

Build the gateway (racing profile):
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing
  cmake --build build --target iris-gw -j"$(nproc)"

Gateway node (pin workers to physical cores 0-3):
  taskset -c 0-3 ./build/gateway/iris-gw --port 8089 --workers 4

perf counters (target IPC > 2.0, low L1-dcache / iTLB misses):
  perf stat -e cycles,instructions,L1-dcache-load-misses,iTLB-load-misses,cache-misses \
      -p "$(pgrep -x iris-gw)" -- sleep 30

Load-gen node (128 conns, 8 threads, 16x pipelined plaintext):
  wrk -t8 -c128 -d15s --latency -s ./scripts/pipeline.lua http://<GATEWAY_IP>:8089/plaintext
  wrk -t8 -c128 -d15s --latency                            http://<GATEWAY_IP>:8089/json
==================================================
EOF
