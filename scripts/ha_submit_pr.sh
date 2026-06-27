#!/usr/bin/env bash
# Submit IRIS to MDA2AV/HttpArena as frameworks/iris.
#
# Prerequisites:
#   - gh auth login
#   - IRIS tag IRIS_REF pushed (Dockerfile clones iris-ha-gw from GitHub)
#
# Usage:  bash scripts/ha_submit_pr.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKDIR="${TMPDIR:-/tmp}/httparena-iris-pr"
UPSTREAM="https://github.com/MDA2AV/HttpArena.git"
FORK="${HA_FORK:-Cobra007-star-source/HttpArena}"
BRANCH="${BRANCH:-add-iris-cpp}"
IRIS_REF="${IRIS_REF:-v0.3.2-ha}"
GIT_AUTHOR_NAME="${GIT_AUTHOR_NAME:-cobrajavinston-afk}"
GIT_AUTHOR_EMAIL="${GIT_AUTHOR_EMAIL:-cobrajavinston@gmail.com}"

command -v gh >/dev/null 2>&1 || {
  echo "error: install GitHub CLI: brew install gh && gh auth login" >&2
  exit 1
}

if ! gh repo view "$FORK" >/dev/null 2>&1; then
  echo "[fork] creating fork $FORK ..."
  gh repo fork MDA2AV/HttpArena --clone=false
fi

FORK_URL="https://github.com/${FORK}.git"

if [[ -d "$WORKDIR/.git" ]] && git -C "$WORKDIR" rev-parse --verify "$BRANCH" >/dev/null 2>&1; then
  echo "[resume] reusing existing workdir on branch $BRANCH"
  cd "$WORKDIR"
  git checkout "$BRANCH"
  GIT_AUTHOR_NAME="$GIT_AUTHOR_NAME" GIT_AUTHOR_EMAIL="$GIT_AUTHOR_EMAIL" \
  GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME" GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL" \
  git commit --amend --reset-author --no-edit 2>/dev/null || true
  mkdir -p frameworks/iris
  rsync -a "$ROOT/httparena/frameworks/iris/" frameworks/iris/
  git add frameworks/iris/
  if ! git diff --cached --quiet; then
    GIT_AUTHOR_NAME="$GIT_AUTHOR_NAME" GIT_AUTHOR_EMAIL="$GIT_AUTHOR_EMAIL" \
    GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME" GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL" \
    git commit -m "$(cat <<EOF
Add C++/IRIS engine (baseline, pipelined, json, static, json-tls, upload, async-db)

Docker image builds iris-ha-gw from ${IRIS_REF} on github.com/Cobra007-star-source/IRIS.
AGPL-3.0.
EOF
)"
  fi
else
  rm -rf "$WORKDIR"
  GIT_HTTP_VERSION=1.1 git clone --depth 1 "$FORK_URL" "$WORKDIR"
  cd "$WORKDIR"
  git checkout -b "$BRANCH"

  mkdir -p frameworks/iris
  rsync -a "$ROOT/httparena/frameworks/iris/" frameworks/iris/

  git add frameworks/iris/
  git status

  GIT_AUTHOR_NAME="$GIT_AUTHOR_NAME" GIT_AUTHOR_EMAIL="$GIT_AUTHOR_EMAIL" \
  GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME" GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL" \
  git commit -m "$(cat <<EOF
Add C++/IRIS engine (baseline, pipelined, json, static, json-tls, upload, async-db)

Docker image builds iris-ha-gw from ${IRIS_REF} on github.com/Cobra007-star-source/IRIS.
AGPL-3.0.
EOF
)"
fi

git remote set-url origin "$FORK_URL"
TOKEN=$(gh auth token)
git push "https://x-access-token:${TOKEN}@github.com/${FORK}.git" "$BRANCH:$BRANCH"
git branch --set-upstream-to=origin/"$BRANCH" "$BRANCH" 2>/dev/null || true

sleep 2

gh pr create \
  --repo MDA2AV/HttpArena \
  --head "Cobra007-star-source:${BRANCH}" \
  --base main \
  --title "Add C++/IRIS engine (iris-ha-gw)" \
  --body "$(cat <<EOF
## Summary

Adds **IRIS** ([Cobra007-star-source/IRIS](https://github.com/Cobra007-star-source/IRIS), AGPL-3.0) as a C++ \`engine\` framework:

- \`frameworks/iris\` — \`baseline\`, \`pipelined\`, \`limited-conn\`, \`json\`, \`json-tls\`, \`static\`, \`upload\`, \`async-db\`
- Docker image clones IRIS at \`${IRIS_REF}\` and builds \`iris-ha-gw\` (Release, racing profile)

## Design notes

- Thread-per-core epoll/SO_REUSEPORT gateway with fixed per-connection buffers
- \`/json/{count}?m=\` serializes from the mounted dataset per request
- \`/static/*\` serves precompressed \`.br\`/\`.gz\` variants with Linux sendfile from sealed memfd responses
- \`POST /upload\` streams large bodies through a 64 KiB read buffer and returns bytes actually read
- OpenSSL TLS on :8081 for json-tls; async libpq for async-db

## Test plan

- [ ] CI builds \`frameworks/iris\` Docker image
- [ ] \`/validate -f iris\` passes on the self-hosted runner
- [ ] \`/benchmark -f iris --save\`
EOF
)"

echo "Done. PR URL above."
