#!/usr/bin/env bash
# =============================================================================
# scripts/compare.sh
#
# End-to-end horizontal comparison script (IRIS Phase 4 deliverable):
#   1. Release build IRIS (-O3 + native SIMD + LTO)
#   2. Generate flat + nested 1M-record JSON corpora
#   3. Run IRIS Fast Path (flat / nested)
#   4. Run simdjson pure-parse baseline (flat / nested)
#   5. Run ajv (Node.js) industry reference
#   6. Print aligned table + ASCII bar chart
#
# Usage:
#   ./scripts/compare.sh                # 1M records, 3 iterations
#   ./scripts/compare.sh 500000 5       # 500K records, 5 iterations
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

COUNT="${1:-1000000}"
ITERS="${2:-3}"
BUILD_DIR="${BUILD_DIR:-build}"
CORPUS_DIR="${CORPUS_DIR:-$ROOT/.corpus}"

c_cyan='\033[36m'; c_off='\033[0m'; c_grn='\033[32m'; c_red='\033[31m'; c_ylw='\033[33m'
step() { printf "${c_cyan}== %s ==${c_off}\n" "$*"; }

step "1. Configure & build IRIS (Release, simdjson on)"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_SIMDJSON=ON >/dev/null
cmake --build "$BUILD_DIR" -j --target gen_corpus iris_validate simdjson_bench validate_bench >/dev/null
echo "  built."

step "2. Generate corpus ($COUNT records → $CORPUS_DIR)"
mkdir -p "$CORPUS_DIR"
"$BUILD_DIR/bench/gen_corpus" "$CORPUS_DIR" "$COUNT"

run_iris() {
    local label="$1" schema="$2" data="$3"
    "$BUILD_DIR/bench/iris_validate" "$schema" "$data" "$ITERS" | tee /dev/stderr | tail -n 1
}
run_simdjson() {
    local data="$1"
    "$BUILD_DIR/bench/simdjson_bench" "$data" "$ITERS" | tee /dev/stderr | tail -n 1
}
run_ajv() {
    local schema="$1" data="$2"
    node bench/ajv_bench.mjs "$schema" "$data" "$ITERS" | tee /dev/stderr | tail -n 1
}

step "3. IRIS — flat schema (4 fields)"
IRIS_FLAT=$(run_iris flat "$CORPUS_DIR/flat/schema.json" "$CORPUS_DIR/flat/data.jsonl")

step "4. IRIS — nested schema (object + array-of-object)"
IRIS_NEST=$(run_iris nested "$CORPUS_DIR/nested/schema.json" "$CORPUS_DIR/nested/data.jsonl")

step "5. simdjson baseline (pure parse, no validation)"
SDJ_FLAT=$(run_simdjson "$CORPUS_DIR/flat/data.jsonl")
SDJ_NEST=$(run_simdjson "$CORPUS_DIR/nested/data.jsonl")

step "6. ajv reference (Node.js)"
NODE_OK=1
if ! command -v node >/dev/null 2>&1; then
    echo "  node not found, skipping ajv. brew install node / apt-get install nodejs"
    NODE_OK=0
fi
AJV_FLAT=""; AJV_NEST=""
if [[ "$NODE_OK" -eq 1 ]]; then
    pushd bench >/dev/null
    [[ ! -d node_modules/ajv ]] && npm install --silent --no-fund --no-audit
    popd >/dev/null
    AJV_FLAT=$(run_ajv "$CORPUS_DIR/flat/schema.json"   "$CORPUS_DIR/flat/data.jsonl")
    AJV_NEST=$(run_ajv "$CORPUS_DIR/nested/schema.json" "$CORPUS_DIR/nested/data.jsonl")
fi

step "7. Summary"
extract_mops() { echo "$1" | sed -nE 's/.* ([0-9.]+) Mops\/s .*/\1/p'; }
extract_mbps() { echo "$1" | sed -nE 's/.* ([0-9.]+) MiB\/s .*/\1/p'; }
extract_ns()   { echo "$1" | sed -nE 's/.* avg ([0-9.]+) ns.*/\1/p'; }

printf "%-20s | %-10s | %-10s | %-10s\n" "engine / corpus" "Mops/s" "MiB/s" "ns/op"
printf -- "---------------------+------------+------------+------------\n"
row() {
    local lbl="$1" out="$2"
    printf "%-20s | %-10s | %-10s | %-10s\n" "$lbl" \
        "$(extract_mops "$out")" "$(extract_mbps "$out")" "$(extract_ns "$out")"
}
row "iris  flat"     "$IRIS_FLAT"
row "iris  nested"   "$IRIS_NEST"
row "simdjson flat"  "$SDJ_FLAT"
row "simdjson nested" "$SDJ_NEST"
if [[ -n "$AJV_FLAT" ]]; then row "ajv   flat"   "$AJV_FLAT"; fi
if [[ -n "$AJV_NEST" ]]; then row "ajv   nested" "$AJV_NEST"; fi

echo
# ASCII bar chart (Mops/s, normalized to max)
echo "Mops/s bar (■=≈1.0 Mops/s, scaled):"
labels=("iris-flat" "iris-nested" "simdjson-flat" "simdjson-nested")
vals=("$(extract_mops "$IRIS_FLAT")" "$(extract_mops "$IRIS_NEST")"
      "$(extract_mops "$SDJ_FLAT")"  "$(extract_mops "$SDJ_NEST")")
if [[ -n "$AJV_FLAT" ]]; then labels+=("ajv-flat");   vals+=("$(extract_mops "$AJV_FLAT")");   fi
if [[ -n "$AJV_NEST" ]]; then labels+=("ajv-nested"); vals+=("$(extract_mops "$AJV_NEST")"); fi

max=$(printf '%s\n' "${vals[@]}" | awk 'BEGIN{m=0}{if($1+0>m)m=$1+0}END{print m}')
for i in "${!labels[@]}"; do
    v="${vals[$i]}"; [[ -z "$v" ]] && v="0"
    w=$(awk -v v="$v" -v m="$max" 'BEGIN{printf "%d", v/m * 40}')
    bar=$(printf "%${w}s" "" | tr ' ' '#')
    printf "  %-18s %6.2f  %s\n" "${labels[$i]}" "$v" "$bar"
done

# Speedup vs ajv
if [[ -n "$AJV_FLAT" && -n "$AJV_NEST" ]]; then
    sf=$(awk -v a="$(extract_mops "$IRIS_FLAT")" -v b="$(extract_mops "$AJV_FLAT")" \
         'BEGIN{printf "%.2f", a/b}')
    sn=$(awk -v a="$(extract_mops "$IRIS_NEST")" -v b="$(extract_mops "$AJV_NEST")" \
         'BEGIN{printf "%.2f", a/b}')
    printf "\n${c_grn}IRIS  vs ajv:      flat=%sx   nested=%sx${c_off}\n" "$sf" "$sn"
fi
if [[ -n "$SDJ_FLAT" && -n "$SDJ_NEST" ]]; then
    df=$(awk -v a="$(extract_mops "$IRIS_FLAT")" -v b="$(extract_mops "$SDJ_FLAT")" \
         'BEGIN{printf "%.2f", a/b}')
    dn=$(awk -v a="$(extract_mops "$IRIS_NEST")" -v b="$(extract_mops "$SDJ_NEST")" \
         'BEGIN{printf "%.2f", a/b}')
    printf "${c_ylw}IRIS  vs simdjson: flat=%sx   nested=%sx (validation included)${c_off}\n" "$df" "$dn"
fi
