#!/usr/bin/env bash
# build.sh — builds both halves of the system: the C++ engine (via CMake)
# and the Go coordinator (via `go build`). Run this once before
# run_cluster.sh / demo.sh.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Building C++ engine (CMake)"
cmake -S "$ROOT_DIR/engine" -B "$ROOT_DIR/engine/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT_DIR/engine/build" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo "==> Running C++ engine test suite (ctest)"
(cd "$ROOT_DIR/engine/build" && ctest --output-on-failure)

echo "==> Building Go coordinator"
mkdir -p "$ROOT_DIR/bin"
(cd "$ROOT_DIR/coordinator" && go build -o "$ROOT_DIR/bin/dse-coordinator" ./cmd/coordinator)

echo "==> Running Go coordinator unit/API tests"
(cd "$ROOT_DIR/coordinator" && go vet ./... && go test ./...)

echo
echo "Build complete."
echo "  Engine binary:      $ROOT_DIR/engine/build/dse_engine"
echo "  Coordinator binary: $ROOT_DIR/bin/dse-coordinator"
echo "Next: ./scripts/run_cluster.sh"
