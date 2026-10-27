#!/usr/bin/env bash
# stop_cluster.sh — stops whatever run_cluster.sh started.
set -uo pipefail
DATA_DIR=/tmp/dse_cluster

for name in engine1 engine2 coord1; do
  pid_file="$DATA_DIR/$name.pid"
  if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file")"
    if kill "$pid" 2>/dev/null; then
      echo "stopped $name (pid $pid)"
    fi
    rm -f "$pid_file"
  fi
done
