#!/bin/bash
# =============================================================================
# setup_target_env.sh
#
# One-shot bare-metal / enhanced-instance environment bootstrap for IRIS gateway
# benchmarking on Linux. SSH this to a fresh host and run once; it turns the box
# into a "performance proving ground" in a few minutes.
#
# Roles:
#   * Gateway node    : build tools + perf (build & run iris-gw, profile it).
#   * Load-gen node   : wrk (saturate the gateway from a SEPARATE machine).
# Installing both on each host is harmless; use whichever role per machine.
#
# Usage:  scp setup_target_env.sh user@host:~ && ssh user@host 'bash setup_target_env.sh'
# =============================================================================
set -euo pipefail

ensure_cmake() {
    # asmjit (JIT serializer) requires CMake >= 3.24. Amazon Linux 2023 ships 3.22.
    if command -v cmake >/dev/null 2>&1; then
        ver="$(cmake --version | awk 'NR==1{print $3}')"
        major="${ver%%.*}"; rest="${ver#*.}"; minor="${rest%%.*}"
        if [ "$major" -gt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -ge 24 ]; }; then
            echo "  cmake ${ver} OK"
            return 0
        fi
        echo "  cmake ${ver} too old (need >= 3.24 for JIT/asmjit)"
    fi
    echo "  installing CMake 3.29.6 to /usr/local ..."
    ver=3.29.6
    curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${ver}/cmake-${ver}-linux-$(uname -m).tar.gz"
    sudo tar -xzf /tmp/cmake.tar.gz -C /usr/local --strip-components=1
    rm -f /tmp/cmake.tar.gz
    /usr/local/bin/cmake --version | head -1
}

echo "[1/5] Installing base build tools + perf (auto-detecting package manager)..."
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y \
        build-essential cmake clang git curl htop \
        linux-tools-common linux-tools-generic "linux-tools-$(uname -r)" \
        || echo "[warn] some linux-tools packages may be unavailable on this kernel (common on cloud/virt kernels); perf may need a kernel-matched package."
elif command -v dnf >/dev/null 2>&1; then
    # AL2023 ships curl-minimal; pulling the full curl package conflicts.
    sudo dnf install -y gcc gcc-c++ make cmake clang git htop perf openssl-devel libubsan \
        || echo "[warn] perf may be unavailable on this AMI/kernel (VMs often lack a PMU; .metal instances expose hardware counters)."
elif command -v yum >/dev/null 2>&1; then
    sudo yum install -y gcc gcc-c++ make cmake clang git htop perf openssl-devel libubsan \
        || echo "[warn] perf may be unavailable on this AMI/kernel (VMs often lack a PMU; .metal instances expose hardware counters)."
else
    echo "[error] no supported package manager found (need apt-get, dnf, or yum)." >&2
    exit 1
fi

echo "[2/5] Ensuring CMake >= 3.24 (required by asmjit JIT backend)..."
ensure_cmake

echo "[3/5] Building and installing wrk (load-generator node)..."
if ! command -v wrk >/dev/null 2>&1; then
    tmpdir="$(mktemp -d)"
    git clone --depth 1 https://github.com/wg/wrk.git "$tmpdir/wrk"
    # Bundled OpenSSL build fails on AL2023 (missing perl FindBin); use system libs.
    make -C "$tmpdir/wrk" WITH_OPENSSL=/usr -j"$(nproc)"
    sudo cp "$tmpdir/wrk/wrk" /usr/local/bin/
    rm -rf "$tmpdir"
else
    echo "  wrk already present: $(command -v wrk)"
fi

echo "[4/5] Tuning kernel network stack for high concurrency..."
sudo sysctl -w net.core.somaxconn=65535            || true
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535  || true
sudo sysctl -w net.core.rmem_max=16777216          || true
sudo sysctl -w net.core.wmem_max=16777216          || true
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535" || true

echo "[5/5] Verifying perf availability..."
perf --version || echo "[warn] perf not runnable; install the kernel-matched linux-tools package for accurate counters."

cat <<'EOF'
==================================================
IRIS proving ground is configured.

Build the gateway (racing profile + JIT /json serializer):
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_GATEWAY=ON \
        -DIRIS_PROFILE=racing -DIRIS_ENABLE_JIT=ON
  cmake --build build --target iris-gw -j"$(nproc)"

Gateway node (pin workers to cores 0-7; load-gen pinned to 8-15 on a 16-vCPU box):
  taskset -c 0-7 ./build/gateway/iris-gw --port 8089 --workers 8

perf counters (target IPC > 2.0, low L1-dcache / iTLB misses):
  perf stat -e cycles,instructions,L1-dcache-load-misses,iTLB-load-misses,cache-misses \
      -p "$(pgrep -x iris-gw)" -- sleep 30

Load-gen node (128 conns, 8 threads, 16x pipelined plaintext):
  wrk -t8 -c128 -d15s --latency -s ./scripts/pipeline.lua http://<GATEWAY_IP>:8089/plaintext
  wrk -t8 -c128 -d15s --latency                            http://<GATEWAY_IP>:8089/json
==================================================
EOF
