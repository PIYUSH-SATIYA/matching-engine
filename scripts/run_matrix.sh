#!/usr/bin/env bash
# Full 2x2x2 integrated matrix:
#   queue {mutex, spsc} x scheduling {default, pin} x network {epoll, uring}
# plus unthrottled throughput runs for every configuration.
#
# usage: run_matrix.sh [--force] [--no-throughput]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=${RESULTS:-"$ROOT/results"}
LAT_RUNS=${ME_LAT_RUNS:-10}
LAT_ORDERS=${ME_LAT_ORDERS:-1010000}
LAT_RATE=${ME_LAT_RATE:-200000}
THR_RUNS=${ME_THR_RUNS:-5}
THR_ORDERS=${ME_THR_ORDERS:-2000000}

FORCE=0
DO_THROUGHPUT=1
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=1 ;;
    --no-throughput) DO_THROUGHPUT=0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [ "$FORCE" -eq 1 ]; then
  for d in "$RESULTS"/raw/q-*; do
    [ -d "$d" ] || continue
    rm -f "$d/runs.csv" "$d"/run-*.log "$d"/samples-run1.bin
  done
fi

# config|queue|book|net|sched
CONFIGS=(
  "q-spsc_sched-pin_net-epoll|spsc|fast-slab|epoll|pin"
  "q-spsc_sched-pin_net-uring|spsc|fast-slab|uring|pin"
  "q-spsc_sched-default_net-epoll|spsc|fast-slab|epoll|default"
  "q-spsc_sched-default_net-uring|spsc|fast-slab|uring|default"
  "q-mutex_sched-pin_net-epoll|mutex|fast-slab|epoll|pin"
  "q-mutex_sched-pin_net-uring|mutex|fast-slab|uring|pin"
  "q-mutex_sched-default_net-epoll|mutex|fast-slab|epoll|default"
  "q-mutex_sched-default_net-uring|mutex|fast-slab|uring|default"
)

existing_rows() {
  local csv=$1 config=$2 kind=$3
  [ -f "$csv" ] || { echo 0; return; }
  awk -F, -v c="$config" -v k="$kind" '$1==c && $3==k {n++} END{print n+0}' "$csv"
}

run_number_present() {
  local csv=$1 config=$2 kind=$3 run=$4
  [ -f "$csv" ] || return 1
  awk -F, -v c="$config" -v k="$kind" -v r="$run" \
    '$1==c && $3==k && $2==r {found=1} END{exit !found}' "$csv"
}

run_kind() {
  local config=$1 queue=$2 book=$3 net=$4 sched=$5 kind=$6 runs=$7 orders=$8 rate=$9
  local csv="$RESULTS/raw/$config/runs.csv"
  local samples=""
  local done_count
  done_count=$(existing_rows "$csv" "$config" "$kind")
  if [ "$done_count" -ge "$runs" ]; then
    echo "== $config [$kind] already complete ($done_count/$runs), skipping"
    return
  fi
  for ((r = 1; r <= runs; ++r)); do
    if run_number_present "$csv" "$config" "$kind" "$r"; then
      echo "== $config [$kind] run $r already present, skipping"
      continue
    fi
    samples=""
    if [ "$kind" = "latency" ] && [ "$r" -eq 1 ]; then
      samples="$RESULTS/raw/$config/samples-run1.bin"
    fi
    echo "== $config [$kind] run $r/$runs (orders=$orders rate=$rate)"
    "$ROOT/scripts/run_one.sh" "$config" "$queue" "$book" "$net" "$sched" \
      "$kind" "$r" "$orders" "$rate" "$samples"
  done
}

for entry in "${CONFIGS[@]}"; do
  IFS='|' read -r config queue book net sched <<<"$entry"
  run_kind "$config" "$queue" "$book" "$net" "$sched" latency \
    "$LAT_RUNS" "$LAT_ORDERS" "$LAT_RATE"
  if [ "$DO_THROUGHPUT" -eq 1 ]; then
    run_kind "$config" "$queue" "$book" "$net" "$sched" throughput \
      "$THR_RUNS" "$THR_ORDERS" 0
  fi
done

echo "matrix complete: $RESULTS/raw"
