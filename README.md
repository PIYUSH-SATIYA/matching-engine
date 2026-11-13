# Low-Latency Matching Engine

A single-threaded price-time-priority matching engine fed by a network thread through
either a mutex ring buffer or a lock-free SPSC ring buffer, with an optional slab
allocator and two Linux network-ingestion backends (epoll and io_uring).

The project is an operating-systems experiment: hold the matching logic fixed and
change one OS mechanism at a time (synchronization, memory allocation, CPU
scheduling, I/O), then measure the latency distribution.

## Architecture

```
 client (load_client, pinned)
        │ 40-byte messages over TCP
        ▼
 network thread: epoll or io_uring
        │ push(Order)
        ▼
 queue: mutex ring buffer or lock-free SPSC ring buffer
        │ pop()
        ▼
 engine thread: MatchingEngine<Book, Sink>
        ├─ OrderBookFast: direct-indexed price array + intrusive levels
        │                 + open-addressing id hash + slab (or heap) nodes
        └─ OrderBook (V1): std::map + std::list reference implementation
```

Each order carries a `CLOCK_MONOTONIC` timestamp taken by the client immediately
before the batch is sent. The engine stops the clock after the order is matched or
rested, so the reported latency is client-send → engine-complete. Client and server
must run on the same host.

## Build and test

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
ctest --test-dir build-rel --output-on-failure
```

Sanitizer runs are separate builds because ASan and TSan cannot coexist:

```bash
cmake -S . -B build-asan -DENABLE_SANITIZERS=ON && cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan -j
ctest --test-dir build-tsan -R 'ring_buffer|spsc' --output-on-failure
```

`scripts/verify.sh` automates all of the above plus an end-to-end epoll/io_uring
checksum comparison.

## Run one measurement

```bash
# server
./build-rel/matching_engine --port 9000 --queue spsc --book fast-slab \
  --net epoll --sched pin --cpu-engine 2 --warmup 10000 \
  --stats results/raw/demo/runs.csv --config demo --run 1 \
  --exit-on-disconnect

# client (pinned away from the engine core)
taskset -c 6 ./build-rel/load_client 127.0.0.1 9000 1010000 42 200000
```

The server exits when the client disconnects (with `--exit-on-disconnect`) and writes
one CSV row per run.

## Reproduce the experiments

```bash
scripts/env.sh                 # results/env.txt
scripts/run_matrix.sh          # 2 queues × 2 scheduling × 2 networks, 10 latency runs + 5 throughput runs each
scripts/run_experiments.sh     # allocator isolation (slab/heap/V1) + SCHED_FIFO probe
scripts/run_components.sh      # no-network floors: single-threaded book, queue+engine pipeline
scripts/run_probe.sh           # ingress attribution + client/network pinning probes
scripts/analyze.py             # results/summary.csv, summary.md, analysis.json, plots/*.svg
```

Useful overrides:

| Variable | Default | Meaning |
|---|---|---|
| `ME_PORT` | `9000` | server port |
| `ME_WARMUP` | `10000` | discarded orders per run |
| `ME_LAT_RUNS` | `10` | latency repetitions per config |
| `ME_LAT_ORDERS` | `1010000` | orders per latency run (10k warmup + 1M measured) |
| `ME_LAT_RATE` | `200000` | open-loop offered rate (orders/s) |
| `ME_THR_RUNS` | `5` | throughput repetitions |
| `ME_THR_ORDERS` | `2000000` | orders per throughput run |
| `ME_CPU_ENGINE` | `2` | engine core for `--sched pin` |
| `ME_CPU_CLIENT` | `6` | client core |
| `ME_PIN_NET` | `0` | set to `1` to also pin the network thread |
| `ME_SEED` | `42` | workload seed |
| `ME_INGEST_STATS` | `1` for latency runs | count orders older than 1 ms at parse time |

`scripts/run_matrix.sh --force` discards previous results. Without `--force`, completed
runs are skipped, so an interrupted matrix resumes.

## Configuration flags

| Flag | Values | Effect |
|---|---|---|
| `--queue` | `mutex`, `spsc` | synchronization mechanism |
| `--book` | `v1`, `fast-slab`, `fast-heap` | reference book, optimized book with slab nodes, optimized book with `new`/`delete` nodes |
| `--net` | `epoll`, `uring` | network backend (`uring` requires liburing at build time) |
| `--sched` | `default`, `pin`, `rt` | default scheduler, engine pinned, engine pinned + `SCHED_FIFO` |
| `--cpu-engine` | core id | pin target for `pin`/`rt` |
| `--cpu-net` | core id | optional network-thread pin (`-1` = floating) |
| `--warmup` | count | discard the first N latency samples |
| `--samples` | path | raw little-endian `u64` latency samples |
| `--stats` | path | append one CSV row per run |
| `--exit-on-disconnect` | flag | stop after the last client disconnects |

## Measurement caveats

The results in `docs/matching-engine-project-doc.md` were collected on the host
described in `results/env.txt`. Important limitations:

- The CPU governor is `powersave` and turbo is enabled; the benchmark cannot change
  them without root.
- `SCHED_FIFO` is denied to the benchmark user (`CAP_SYS_NICE` unavailable). The RT
  configuration is reported as blocked, not fabricated.
- `perf` and `strace` are unavailable (`perf_event_paranoid=2`), so mechanism
  evidence comes from in-process counters: `getrusage(RUSAGE_THREAD)` for context
  switches and CPU time, `sched_getcpu()` for migrations, and per-backend syscall /
  submission / completion counters.
- SMT is enabled and `isolcpus` is not used, so a pinned thread still shares its core
  with whatever else the host schedules there. This is the dominant caveat: the host
  is a desktop session, not a dedicated test box (`results/env.txt` records the load
  and the top CPU consumers at capture time).
- The tail is attributed with in-process instrumentation rather than guesswork. Every
  order is stamped twice inside the server: `enqueue_time` on the network thread after
  parsing, and completion on the engine thread. The CSV therefore contains both the
  arrival component (client send → parse) and the service component (parse → matched),
  plus cumulative run-queue wait for both threads from
  `/proc/thread-self/schedstat` (`engine_runq_ns`, `net_runq_ns`).
- The network thread is left floating by default. Pinning it to one core made it
  sensitive to unrelated host load and increased the tail (measurably: p99.9 158 µs
  → 831 µs with 0.07 % of orders arriving >1 ms late).
- All networking is loopback. The measurements quantify kernel/user-boundary and
  scheduling effects; they are not a NIC or cross-host benchmark.
- `results/summary.md` separates *clean* runs from host-contended runs. A run is
  contended when at least one order was already >1 ms old at parse time; those runs
  are listed individually rather than deleted. `results/host-isolation.txt` records a
  one-off controlled check showing the same configuration move from 2.40 ms to
  142 µs p99.9 when the host load was confined, which is what justifies treating the
  outliers as ingress delay rather than pipeline behaviour.

## Layout

```
include/      headers: engine, books, queues, wire format, recency, affinity
src/          matching engine app, network backends, V1 book, mutex queue
tools/        load_client
tests/        unit, differential, parser and concurrency tests
bench/        standalone book and pipeline microbenchmarks
scripts/      environment capture, run harness, analysis, verification
results/      env.txt, summary.csv/md, plots, raw per-run CSVs
docs/         proposal, contracts, iteration plan, final report
```
