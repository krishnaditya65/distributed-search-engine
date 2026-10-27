#!/usr/bin/env bash
# run_cluster.sh — starts a small local cluster for manual testing/demoing:
#   - 2 C++ engine (data node) processes, ports 7001 and 7002
#   - 1 Go coordinator node (bootstraps a single-node raft "cluster"),
#     REST API on :8090, raft transport on 127.0.0.1:9010
#
# Data and logs are written under /tmp/dse_cluster (wiped on each run, unless
# you pass --keep-data). Run scripts/build.sh first.
#
# Usage: ./scripts/run_cluster.sh [--keep-data]
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_DIR=/tmp/dse_cluster

ENGINE_BIN="$ROOT_DIR/engine/build/dse_engine"
COORD_BIN="$ROOT_DIR/bin/dse-coordinator"

if [[ ! -x "$ENGINE_BIN" || ! -x "$COORD_BIN" ]]; then
  echo "Binaries not found — run ./scripts/build.sh first." >&2
  exit 1
fi

if [[ "${1:-}" != "--keep-data" ]]; then
  rm -rf "$DATA_DIR"
fi
mkdir -p "$DATA_DIR"

echo "==> Starting engine-1 on :7001"
"$ENGINE_BIN" --port=7001 --data-dir="$DATA_DIR/engine1" --node-id=engine-1 --maintenance-interval-sec=10 \
  > "$DATA_DIR/engine1.log" 2>&1 &
echo $! > "$DATA_DIR/engine1.pid"

echo "==> Starting engine-2 on :7002"
"$ENGINE_BIN" --port=7002 --data-dir="$DATA_DIR/engine2" --node-id=engine-2 --maintenance-interval-sec=10 \
  > "$DATA_DIR/engine2.log" 2>&1 &
echo $! > "$DATA_DIR/engine2.pid"

sleep 1

echo "==> Starting coordinator on :8090 (raft :9010, bootstrapping single-node cluster)"
"$COORD_BIN" -bootstrap -node-id=coord1 -http-addr=127.0.0.1:8090 \
  -raft-addr=127.0.0.1:9010 -raft-dir="$DATA_DIR/coord1/raft" \
  -engine-nodes=localhost:7001,localhost:7002 \
  > "$DATA_DIR/coord1.log" 2>&1 &
echo $! > "$DATA_DIR/coord1.pid"

sleep 2

echo
echo "Cluster is up:"
echo "  Coordinator REST API: http://127.0.0.1:8090"
echo "  Engine nodes:          localhost:7001, localhost:7002 (gRPC)"
echo "  Logs:                  $DATA_DIR/{engine1,engine2,coord1}.log"
echo
curl -s http://127.0.0.1:8090/health && echo
echo
echo "Try: ./scripts/demo.sh"
echo "Stop everything with: ./scripts/stop_cluster.sh"
