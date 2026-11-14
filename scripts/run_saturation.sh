#!/usr/bin/env bash
# Queue saturation sweep (proposal Experiment 5).
#
# Drives the integrated pipeline with a fixed offered rate and increases that
# rate until the queue can no longer hand orders over immediately.  At each
# point it records the end-to-end latency percentiles and the number of
# blocking push/pop calls the queue had to perform (queue_full_waits /
# queue_empty_waits), which is exactly "queue-full events" and "queue-empty
# events" from the iteration plan.
#
# kind=saturation keeps these runs out of the latency/throughput tables, and
# each rate gets its own config label so runs.csv stays append-only.
#
# usage: run_saturation.sh [reps]
# Environment: ME_SAT_QUEUES (default "spsc mutex"), ME_SAT_RATES,
#              ME_SAT_WINDOW_S (default 2.0), ME_SAT_MAX_ORDERS (default 3000000)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=${RESULTS:-"$ROOT/results"}
REPS=${1:-${ME_SAT_REPS:-3}}
QUEUES=${ME_SAT_QUEUES:-"spsc mutex"}
RATES=${ME_SAT_RATES:-"50000 100000 200000 400000 800000 1600000 3200000 6400000 0"}
WINDOW=${ME_SAT_WINDOW_S:-2.0}
MAX_ORDERS=${ME_SAT_MAX_ORDERS:-3000000}
WARMUP=${ME_WARMUP:-10000}
NET=${ME_SAT_NET:-epoll}
SCHED=${ME_SAT_SCHED:-pin}
BOOK=${ME_SAT_BOOK:-fast-slab}

# The server keeps every measured sample in memory (LatencyRecorder capacity is
# 4M).  Hold total orders below the cap by shortening the window at high rates
# instead of running taller than the recorder.
orders_for() {
  local rate=$1
  if [ "$rate" -eq 0 ]; then
    echo 2000000
    return
  fi
  awk -v r="$rate" -v w="$WINDOW" -v cap="$MAX_ORDERS" -v warm="$WARMUP" \
    'BEGIN { n = r * w; if (n > cap) n = cap; printf "%d\n", n + warm }'
}

label_for() {
  local queue=$1 rate=$2
  if [ "$rate" -eq 0 ]; then
    echo "sat-$queue-unthrottled"
  else
    echo "sat-$queue-rate-$rate"
  fi
}

echo "saturation sweep: queues=[$QUEUES] rates=[$RATES] reps=$REPS window=${WINDOW}s"
for queue in $QUEUES; do
  for rate in $RATES; do
    config=$(label_for "$queue" "$rate")
    orders=$(orders_for "$rate")
    for ((r = 1; r <= REPS; ++r)); do
      csv="$RESULTS/raw/$config/runs.csv"
      if [ -f "$csv" ] && awk -F, -v c="$config" -v r="$r" \
           '$1==c && $2==r {f=1} END{exit !f}' "$csv"; then
        echo "== $config run $r already present, skipping"
        continue
      fi
      echo "== $config run $r/$REPS orders=$orders rate=$rate"
      "$ROOT/scripts/run_one.sh" "$config" "$queue" "$BOOK" "$NET" "$SCHED" \
        saturation "$r" "$orders" "$rate"
    done
  done
done

echo "saturation runs complete: $RESULTS/raw/sat-*"
