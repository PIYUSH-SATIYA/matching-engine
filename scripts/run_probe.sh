#!/usr/bin/env bash
# Mechanism probes.  These are not part of the main matrix: they exist to
# attribute the residual p99.9 tail to a specific stage of the pipeline.
#
# All probes run with ME_INGEST_STATS=1, which compares each parsed order's
# client CLOCK_MONOTONIC stamp with the time the network thread parsed it.
#
#   probe-ingest-epoll   : baseline ingestion instrumentation (epoll)
#   probe-ingest-uring   : baseline ingestion instrumentation (io_uring)
#   probe-client-free    : client not pinned (is the tail client scheduling?)
#   probe-net-pinned     : network thread pinned to CPU 4 (was it migration?)
#
# usage: run_probe.sh [reps]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=${RESULTS:-"$ROOT/results"}
REPS=${1:-${ME_PROBE_REPS:-3}}
ORDERS=${ME_PROBE_ORDERS:-1010000}
RATE=${ME_PROBE_RATE:-200000}

run_probe() {
  local config=$1 net=$2 pin_net=$3 pin_client=$4
  for ((r = 1; r <= REPS; ++r)); do
    if [ -f "$RESULTS/raw/$config/runs.csv" ] && awk -F, -v c="$config" -v r="$r" \
      '$1==c && $2==r {f=1} END{exit !f}' "$RESULTS/raw/$config/runs.csv"; then
      echo "== $config run $r already present, skipping"
      continue
    fi
    echo "== $config run $r/$REPS"
    ME_INGEST_STATS=1 ME_PIN_NET="$pin_net" ME_PIN_CLIENT="$pin_client" \
      "$ROOT/scripts/run_one.sh" "$config" spsc fast-slab "$net" pin \
      latency "$r" "$ORDERS" "$RATE"
  done
}

run_probe probe-ingest-epoll epoll 0 1
run_probe probe-ingest-uring uring 0 1
run_probe probe-client-free epoll 0 0
run_probe probe-net-pinned epoll 1 1

echo "probes complete: $RESULTS/raw/probe-*"
