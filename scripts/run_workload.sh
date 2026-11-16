#!/usr/bin/env bash
# Run profile-driven workload repetitions without touching results/raw.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-"$ROOT/build-rel"}; RESULTS=${RESULTS:-"$ROOT/results/workloads"}
PORT=${ME_PORT:-9000}; QUEUE=${ME_QUEUE:-spsc}; BOOK=${ME_BOOK:-fast-slab}
NET=${ME_NET:-epoll}; SCHED=${ME_SCHED:-pin}; CPU_ENGINE=${ME_CPU_ENGINE:-2}
CPU_CLIENT=${ME_CPU_CLIENT:-6}; ORDERS=${ME_WORKLOAD_ORDERS:-1010000}
RATE=${ME_WORKLOAD_RATE:-200000}; RUNS=${ME_WORKLOAD_RUNS:-1}; SEED=${ME_SEED:-42}
WARMUP=${ME_WARMUP:-10000}; PROFILE=${ME_WORKLOAD_PROFILE:-market-like}
INVALID_RATE=${ME_INVALID_RATE:-0}; DUPLICATE_RATE=${ME_DUPLICATE_RATE:-0}
BUY_PROB=${ME_BUY_PROBABILITY:-0.5}; FORCE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) PROFILE=$2; shift 2;;
    --runs) RUNS=$2; shift 2;;
    --orders) ORDERS=$2; shift 2;;
    --rate) RATE=$2; shift 2;;
    --seed) SEED=$2; shift 2;;
    --invalid-rate) INVALID_RATE=$2; shift 2;;
    --duplicate-rate) DUPLICATE_RATE=$2; shift 2;;
    --buy-probability) BUY_PROB=$2; shift 2;;
    --queue) QUEUE=$2; shift 2;;
    --book) BOOK=$2; shift 2;;
    --net) NET=$2; shift 2;;
    --sched) SCHED=$2; shift 2;;
    --force) FORCE=1; shift;;
    --help) echo "usage: $0 --profile NAME [--runs N --orders N --rate R]"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
[[ -x "$BIN/matching_engine" ]] || { echo "missing $BIN/matching_engine" >&2; exit 1; }
[[ -x "$BIN/workload_client" ]] || { echo "missing $BIN/workload_client" >&2; exit 1; }
STAMP=${ME_WORKLOAD_TIMESTAMP:-$(date +%Y%m%dT%H%M%S)}
ROOT_OUT="$RESULTS/$PROFILE/$STAMP"
if [[ -e "$ROOT_OUT" && "$FORCE" != 1 ]]; then echo "output exists: $ROOT_OUT" >&2; exit 1; fi
mkdir -p "$ROOT_OUT"
for ((run=1; run<=RUNS; run++)); do
  OUT="$ROOT_OUT/run-$run"; mkdir -p "$OUT"; CSV="$OUT/runs.csv"
  args=(--port "$PORT" --queue "$QUEUE" --book "$BOOK" --net "$NET" --sched "$SCHED"
        --warmup "$WARMUP" --stats "$CSV" --config "workload-$PROFILE-$QUEUE"
        --run "$run" --kind workload --offered-rate "$RATE" --exit-on-disconnect)
  [[ "$SCHED" == default ]] || args+=(--cpu-engine "$CPU_ENGINE")
  export ME_INGEST_STATS=1
  timeout 240 "$BIN/matching_engine" "${args[@]}" >"$OUT/server.log" 2>&1 & server_pid=$!
  client=("$BIN/workload_client" --host 127.0.0.1 --port "$PORT" --orders "$ORDERS"
          --rate "$RATE" --seed "$((SEED + run - 1))" --profile "$PROFILE"
          --invalid-rate "$INVALID_RATE" --duplicate-rate "$DUPLICATE_RATE"
          --buy-probability "$BUY_PROB" --output "$OUT")
  set +e
  if [[ "${ME_PIN_CLIENT:-1}" == 1 ]]; then
    timeout 240 taskset -c "$CPU_CLIENT" "${client[@]}" >"$OUT/client.log" 2>&1
  else
    timeout 240 "${client[@]}" >"$OUT/client.log" 2>&1
  fi
  client_rc=$?; wait "$server_pid"; server_rc=$?; set -e
  [[ $client_rc == 0 ]] || { echo "client failed; see $OUT/client.log" >&2; exit 1; }
  [[ $server_rc == 0 ]] || { echo "server failed; see $OUT/server.log" >&2; exit 1; }
done
cat >"$ROOT_OUT/run-config.txt" <<EOF
profile=$PROFILE
runs=$RUNS
orders=$ORDERS
rate=$RATE
seed=$SEED
queue=$QUEUE
book=$BOOK
net=$NET
sched=$SCHED
invalid_rate=$INVALID_RATE
duplicate_rate=$DUPLICATE_RATE
buy_probability=$BUY_PROB
EOF
python3 "$ROOT/scripts/analyze_workload.py" --input "$ROOT_OUT"
echo "workload results: $ROOT_OUT"
