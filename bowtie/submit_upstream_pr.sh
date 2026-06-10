#!/usr/bin/env bash
# Submit IRIS to bowtie-json-schema/bowtie as implementations/cpp-iris.
#
# Prerequisites:
#   - gh auth login   (or fork manually at https://github.com/bowtie-json-schema/bowtie/fork)
#   - IRIS tag v0.1.0 pushed (Dockerfile clones it)
#
# Usage:  bash bowtie/submit_upstream_pr.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKDIR="${TMPDIR:-/tmp}/bowtie-cpp-iris-pr"
UPSTREAM="https://github.com/bowtie-json-schema/bowtie.git"
FORK="${BOWTIE_FORK:-Cobra007-star-source/bowtie}"

command -v gh >/dev/null 2>&1 || {
  echo "error: install GitHub CLI: brew install gh && gh auth login" >&2
  exit 1
}

rm -rf "$WORKDIR"
git clone --depth 1 "$UPSTREAM" "$WORKDIR"
cd "$WORKDIR"
git checkout -b add-cpp-iris

mkdir -p implementations/cpp-iris
cp "$ROOT/bowtie/Dockerfile" implementations/cpp-iris/Dockerfile
git add implementations/cpp-iris/Dockerfile
git commit -m "$(cat <<'EOF'
Add cpp-iris implementation (IRIS JSON Schema validator)

IRIS reaches 1295/1295 on draft 2020-12 via a minimal Alpine image that
clones IRIS@v0.1.0 and builds the bowtie_iris IHOP harness (fast/slow
auto-fallback + case.registry $ref resolution). License: AGPL-3.0.
EOF
)"

if ! gh repo view "$FORK" >/dev/null 2>&1; then
  echo "[fork] creating fork $FORK ..."
  gh repo fork bowtie-json-schema/bowtie --clone=false --remote=false
fi

git remote add fork "git@github.com:${FORK}.git" 2>/dev/null || true
git push -u fork add-cpp-iris

gh pr create \
  --repo bowtie-json-schema/bowtie \
  --head "${FORK}:add-cpp-iris" \
  --base main \
  --title "Add cpp-iris (IRIS JSON Schema validator, draft 2020-12 100%)" \
  --body "$(cat <<'EOF'
## Summary

Adds **cpp-iris**, a Bowtie harness for [IRIS](https://github.com/Cobra007-star-source/IRIS) — a high-performance JSON Schema validator with dual fast/slow engines.

- **Image**: clones IRIS `v0.1.0`, builds the `bowtie_iris` IHOP binary (validator core only; no gateway).
- **Conformance**: 1295/1295 on the official draft 2020-12 test suite (verified locally via `bench/bowtie_local_check.py`).
- **Harness**: routes through `Validator::from_schema_json` (auto fast↔slow fallback) and consumes `case.registry` for `$ref` resolution.
- **License**: AGPL-3.0 (reported in the `start` response).

## Test plan

- [ ] CI builds `implementations/cpp-iris` image
- [ ] Bowtie suite run on draft 2020-12 shows 1295/1295
EOF
)"

echo "Done. PR URL above."
