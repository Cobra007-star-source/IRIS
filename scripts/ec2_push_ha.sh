#!/usr/bin/env bash
# Push IRIS to EC2 server node and run HttpArena server prepare (build iris-ha-gw).
#
# Usage:
#   ./scripts/ec2_push_ha.sh ec2-user@<server-public-ip>
#   SERVER=ec2-user@1.2.3.4 CLIENT=ec2-user@5.6.7.8 ./scripts/ec2_push_ha.sh
set -euo pipefail

SERVER=${1:-${SERVER:?usage: ec2_push_ha.sh user@server-ip  (or set SERVER=)}}
CLIENT=${CLIENT:-}
KEY=${IRIS_SSH_KEY:-/Users/shaorong/IRIS/iris-v8.pem}
REMOTE_DIR=${IRIS_REMOTE_DIR:-/home/ec2-user/IRIS}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o ServerAliveInterval=30)
RSYNC=(rsync -az --delete --progress
    -e "ssh -i $KEY -o StrictHostKeyChecking=no -o ServerAliveInterval=30")

sync_iris() {
    local host=$1
    echo "=== sync IRIS -> $host:$REMOTE_DIR ==="
    "${RSYNC[@]}" \
        --exclude '.git' \
        --exclude 'build*' \
        --exclude 'node_modules' \
        --exclude '.test-suite' \
        --exclude '.corpus' \
        --exclude 'bench-data' \
        --exclude 'bench/' \
        --exclude 'benchmarks/' \
        --exclude 'sparring/' \
        --exclude 'bowtie/' \
        --exclude 'examples/' \
        --exclude 'tests/' \
        --exclude 'fuzz/' \
        --exclude 'benchmarks_data/' \
        --exclude 'perf.data' \
        --exclude 'FlameGraph' \
        --exclude '.cache' \
        --exclude '*.svg' \
        --exclude '*.pem' \
        "$ROOT/" "$host:$REMOTE_DIR/"
}

echo "=== SERVER: $SERVER ==="
sync_iris "$SERVER"
"${SSH[@]}" "$SERVER" "chmod +x $REMOTE_DIR/scripts/ha_*.sh && bash $REMOTE_DIR/scripts/ha_server_node.sh prepare"

if [[ -n "$CLIENT" ]]; then
    echo ""
    echo "=== CLIENT: $CLIENT ==="
    sync_iris "$CLIENT"
    "${SSH[@]}" "$CLIENT" "chmod +x $REMOTE_DIR/scripts/ha_*.sh && bash $REMOTE_DIR/scripts/ha_client_node.sh --help 2>/dev/null || true"
    echo "Client synced. Install gcannon on first bench run."
fi

echo ""
echo "=== Next steps ==="
echo "1. Server:  ssh -i $KEY $SERVER"
echo "            FRAMEWORK=iris bash $REMOTE_DIR/scripts/ha_server_node.sh run"
echo "2. Client:  ssh -i $KEY ${CLIENT:-<client-host>}"
echo "            TARGET_HOST=<server-private-ip> bash $REMOTE_DIR/scripts/ha_client_node.sh"
echo "3. Repeat with FRAMEWORK=zix|minima-sync|zeemo on server, re-run client."
