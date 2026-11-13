#!/usr/bin/env bash
# Run exactly one server + one client measurement.
#
# usage: run_one.sh CONFIG QUEUE BOOK NET SCHED KIND RUN ORDERS RATE [SAMPLES]
#   CONFIG  label written into the CSV
#   QUEUE   mutex|spsc
#   BOOK    v1|fast-slab|fast-heap
#   NET     epoll|uring
#   SCHED   default|pin|rt
#   KIND    latency|throughput
#   RUN     1-based repetition number
#   ORDERS  total orders sent (warmup included)
#   RATE    orders/sec; 0 = unthrottled
#   SAMPLES optional raw latency output path
#
# Environment: ME_PORT (default 9000), ME_WARMUP (default 10000),
#              ME_CPU_ENGINE (2), ME_CPU_NET (4), ME_CPU_CLIENT (6),
#              BIN (default build-rel)
set -euo pipefail

if [ "$#" -lt 9 ]; then
  echo "usage: $0 CONFIG QUEUE BOOK NET SCHED KIND RUN ORDERS RATE [SAMPLES]" >&2
  exit 2
fi

CONFIG=$1 QUEUE=$2 BOOK=$3 NET=$4 SCHED=$5 KIND=$6 RUN=$7 ORDERS=$8 RATE=$9
SAMPLES=${10:-}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-"$ROOT/build-rel"}
RESULTS=${RESULTS:-"$ROOT/results"}
PORT=${ME_PORT:-9000}
WARMUP=${ME_WARMUP:-10000}
CPU_ENGINE=${ME_CPU_ENGINE:-2}
CPU_NET=${ME_CPU_NET:-4}
CPU_CLIENT=${ME_CPU_CLIENT:-6}
SEED=${ME_SEED:-42}

DIR="$RESULTS/raw/$CONFIG"
mkdir -p "$DIR"
CSV="$DIR/runs.csv"
LOG="$DIR/run-$RUN.log"
CLIENT_LOG="$DIR/run-$RUN.client.log"

[ -x "$BIN/matching_engine" ] || { echo "missing $BIN/matching_engine" >&2; exit 1; }
[ -x "$BIN/load_client" ] || { echo "missing $BIN/load_client" >&2; exit 1; }

SERVER_ARGS=(--port "$PORT" --queue "$QUEUE" --book "$BOOK" --net "$NET"
             --sched "$SCHED" --warmup "$WARMUP" --stats "$CSV"
             --config "$CONFIG" --run "$RUN" --kind "$KIND"
             --exit-on-disconnect)
if [ "$SCHED" != "default" ]; then
  SERVER_ARGS+=(--cpu-engine "$CPU_ENGINE")
  # By default the network thread is left floating: pinning it to one core
  # made it a victim of whatever else the host scheduled there.  Set
  # ME_PIN_NET=1 to include it in the pinned configuration.
  if [ "${ME_PIN_NET:-0}" = "1" ]; then
    SERVER_ARGS+=(--cpu-net "$CPU_NET")
  fi
fi
if [ -n "$SAMPLES" ] && [ "$SAMPLES" != "-" ]; then
  mkdir -p "$(dirname "$SAMPLES")"
  SERVER_ARGS+=(--samples "$SAMPLES")
fi

ROWS_BEFORE=0
if [ -f "$CSV" ]; then
  ROWS_BEFORE=$(awk -F, -v c="$CONFIG" -v r="$RUN" '$1==c && $2==r {n++} END{print n+0}' "$CSV")
fi

start=$(date +%s.%N)
timeout 180 "$BIN/matching_engine" "${SERVER_ARGS[@]}" >"$LOG" 2>&1 &
SERVER_PID=$!

set +e
timeout 180 taskset -c "$CPU_CLIENT" "$BIN/load_client" \
  127.0.0.1 "$PORT" "$ORDERS" "$SEED" "$RATE" >"$CLIENT_LOG" 2>&1
CLIENT_RC=$?
wait "$SERVER_PID"
SERVER_RC=$?
set -e
end=$(date +%s.%N)

if [ "$CLIENT_RC" -ne 0 ]; then
  echo "[$CONFIG run $RUN] client failed (rc=$CLIENT_RC)" >&2
  tail -5 "$CLIENT_LOG" >&2 || true
  exit 1
fi
if [ "$SERVER_RC" -ne 0 ]; then
  echo "[$CONFIG run $RUN] server failed (rc=$SERVER_RC)" >&2
  tail -15 "$LOG" >&2 || true
  exit 1
fi

ROWS_AFTER=0
if [ -f "$CSV" ]; then
  ROWS_AFTER=$(awk -F, -v c="$CONFIG" -v r="$RUN" '$1==c && $2==r {n++} END{print n+0}' "$CSV")
fi
if [ "$ROWS_AFTER" -le "$ROWS_BEFORE" ]; then
  echo "[$CONFIG run $RUN] no CSV row written" >&2
  tail -15 "$LOG" >&2 || true
  exit 1
fi

awk -F, -v c="$CONFIG" -v r="$RUN" '$1==c && $2==r {printf "[%s run %s] throughput=%s/s p50=%s p99=%s p99.9=%s trades=%s wall=%.2fs\n",$1,$2,$12,$13,$15,$16,$8,'"$end"' - '"$start"'}' "$CSV" | tail -1
