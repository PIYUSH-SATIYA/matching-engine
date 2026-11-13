#!/usr/bin/env bash
# Component experiments not covered by the integrated matrix:
#   * allocator isolation: fast-slab vs fast-heap vs V1 (map/list reference)
#   * SCHED_FIFO capability probe (expected to fail without CAP_SYS_NICE)
#
# Queue, scheduling (default/pin) and network comparisons come from run_matrix.sh.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=${RESULTS:-"$ROOT/results"}
BIN=${BIN:-"$ROOT/build-rel"}
LAT_RUNS=${ME_LAT_RUNS:-10}
LAT_ORDERS=${ME_LAT_ORDERS:-1010000}
LAT_RATE=${ME_LAT_RATE:-200000}
THR_RUNS=${ME_THR_RUNS:-5}
THR_ORDERS=${ME_THR_ORDERS:-2000000}

mkdir -p "$RESULTS"

run_kind() {
  local config=$1 book=$2 kind=$3 runs=$4 orders=$5 rate=$6
  local csv="$RESULTS/raw/$config/runs.csv"
  for ((r = 1; r <= runs; ++r)); do
    if [ -f "$csv" ] && awk -F, -v c="$config" -v k="$kind" -v r="$r" \
        '$1==c && $3==k && $2==r {f=1} END{exit !f}' "$csv"; then
      echo "== $config [$kind] run $r already present, skipping"
      continue
    fi
    local samples=""
    if [ "$kind" = "latency" ] && [ "$r" -eq 1 ]; then
      samples="$RESULTS/raw/$config/samples-run1.bin"
    fi
    echo "== $config [$kind] run $r/$runs"
    "$ROOT/scripts/run_one.sh" "$config" spsc "$book" epoll pin \
      "$kind" "$r" "$orders" "$rate" "$samples"
  done
}

for book in fast-slab fast-heap v1; do
  config="alloc-$book"
  run_kind "$config" "$book" latency "$LAT_RUNS" "$LAT_ORDERS" "$LAT_RATE"
  run_kind "$config" "$book" throughput "$THR_RUNS" "$THR_ORDERS" 0
done

# SCHED_FIFO probe: record the exact failure instead of pretending it worked.
echo "== probing SCHED_FIFO capability"
RT_LOG="$RESULTS/rt-attempt.txt"
set +e
timeout 10 "$BIN/matching_engine" --port 9500 --queue spsc --book fast-slab \
  --net epoll --sched rt --cpu-engine 2 --cpu-net 4 --warmup 1000 \
  --config rt-probe --run 0 --kind latency >"$RT_LOG" 2>&1
RT_RC=$?
set -e
{
  echo "exit_code=$RT_RC"
  echo "note=SCHED_FIFO requires CAP_SYS_NICE or a realtime RLIMIT; see log below"
  echo "--- log ---"
  cat "$RT_LOG"
} >"$RT_LOG.tmp"
mv "$RT_LOG.tmp" "$RT_LOG"
if [ "$RT_RC" -eq 2 ]; then
  echo "SCHED_FIFO correctly reported as unavailable (rc=2); see $RT_LOG"
else
  echo "warning: RT probe returned rc=$RT_RC (expected 2); see $RT_LOG" >&2
fi

echo "component experiments complete: $RESULTS/raw"
