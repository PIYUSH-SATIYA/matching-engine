#!/usr/bin/env bash
# Full verification: Release + ASan + TSan + end-to-end epoll/io_uring parity.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
JOBS=$(nproc)
TMP=${TMPDIR:-/tmp}/me-verify
mkdir -p "$TMP"

echo "== Release build"
cmake -S . -B build-verify -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-verify -j"$JOBS" >/dev/null
ctest --test-dir build-verify --output-on-failure

echo "== ASan/UBSan build"
cmake -S . -B build-verify-asan -DENABLE_SANITIZERS=ON >/dev/null
cmake --build build-verify-asan -j"$JOBS" >/dev/null
ctest --test-dir build-verify-asan --output-on-failure

echo "== TSan build (ring buffers only: the rest has no shared mutable state)"
cmake -S . -B build-verify-tsan -DENABLE_TSAN=ON >/dev/null
cmake --build build-verify-tsan -j"$JOBS" --target ring_buffer_test spsc_ring_buffer_test >/dev/null
ctest --test-dir build-verify-tsan -R 'ring_buffer_test|spsc_ring_buffer_test' --output-on-failure

echo "== end-to-end parity: epoll vs io_uring"
for net in epoll uring; do
  port=$([ "$net" = epoll ] && echo 9601 || echo 9602)
  csv="$TMP/$net.csv"
  rm -f "$csv"
  timeout 60 ./build-verify/matching_engine --port "$port" --queue spsc \
    --book fast-slab --net "$net" --sched pin --cpu-engine 2 --cpu-net 4 \
    --warmup 1000 --stats "$csv" --config "verify-$net" --run 1 \
    --kind latency --exit-on-disconnect >"$TMP/$net.log" 2>&1 &
  pid=$!
  taskset -c 6 ./build-verify/load_client 127.0.0.1 "$port" 50000 42 0 \
    >"$TMP/$net.client.log" 2>&1
  wait "$pid"
  tail -3 "$TMP/$net.log"
done

read -r etrades eqty enotional < <(awk -F, 'NR==2{print $8,$26,$27}' "$TMP/epoll.csv")
read -r utrades uqty unotional < <(awk -F, 'NR==2{print $8,$26,$27}' "$TMP/uring.csv")
if [ "$etrades" != "$utrades" ] || [ "$eqty" != "$uqty" ] || [ "$enotional" != "$unotional" ]; then
  echo "parity FAILED: epoll=($etrades,$eqty,$enotional) uring=($utrades,$uqty,$unotional)" >&2
  exit 1
fi
echo "parity OK: trades=$etrades qty=$eqty notional=$enotional"
echo "verification complete"
