#!/usr/bin/env bash
# Component micro-benchmarks that isolate the matching core and the queue from
# the network path.  These are the "floor" measurements referenced by the
# report: no sockets, no kernel transitions, no wire parsing.
#
#   bench_order_book  : single thread, book + matching loop only (V1 vs V3)
#   bench_pipeline    : producer thread -> queue -> engine thread (no network)
#
# Results are plain text under results/components/.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-"$ROOT/build-rel"}
RESULTS=${RESULTS:-"$ROOT/results"}
OUT="$RESULTS/components"
mkdir -p "$OUT"

CPU_ENGINE=${ME_CPU_ENGINE:-2}
CPU_CLIENT=${ME_CPU_CLIENT:-6}
REPS=${ME_COMPONENT_REPS:-5}
BOOK_OPS=${ME_BOOK_OPS:-2000000}
PIPE_ORDERS=${ME_PIPE_ORDERS:-1010000}
PIPE_RATE=${ME_PIPE_RATE:-200000}
SEED=${ME_SEED:-42}

for bin in bench_order_book bench_pipeline; do
  [ -x "$BIN/$bin" ] || { echo "missing $BIN/$bin" >&2; exit 1; }
done

echo "== bench_order_book (single-threaded matching floor) x$REPS"
: >"$OUT/bench-order-book.txt"
{
  echo "# bench_order_book ops=$BOOK_OPS seed=$SEED reps=$REPS cpu=$CPU_ENGINE"
  echo "# $(date -Is)"
} >>"$OUT/bench-order-book.txt"
for ((i = 1; i <= REPS; ++i)); do
  echo "--- rep $i ---" | tee -a "$OUT/bench-order-book.txt"
  taskset -c "$CPU_ENGINE" "$BIN/bench_order_book" "$BOOK_OPS" "$SEED" \
    | tee -a "$OUT/bench-order-book.txt"
done

echo "== bench_pipeline (queue + matching, no network) x$REPS"
: >"$OUT/bench-pipeline.txt"
{
  echo "# bench_pipeline orders=$PIPE_ORDERS rate=$PIPE_RATE seed=$SEED reps=$REPS"
  echo "# producer cpu=$CPU_CLIENT engine cpu=$CPU_ENGINE"
  echo "# $(date -Is)"
} >>"$OUT/bench-pipeline.txt"
for ((i = 1; i <= REPS; ++i)); do
  echo "--- rep $i ---" | tee -a "$OUT/bench-pipeline.txt"
  taskset -c "$CPU_CLIENT","$CPU_ENGINE" "$BIN/bench_pipeline" \
    "$PIPE_ORDERS" "$PIPE_RATE" "$CPU_CLIENT" "$CPU_ENGINE" \
    | tee -a "$OUT/bench-pipeline.txt"
done

echo "components written to $OUT"
