# Low-Latency Matching Engine

A single-threaded price-time-priority matching engine fed by a network thread through
either a mutex ring buffer or a lock-free SPSC ring buffer, with an optional slab
allocator and two Linux network-ingestion backends (epoll and io_uring).

The project is an operating-systems experiment: hold the matching logic fixed and
change one OS mechanism at a time (synchronization, memory allocation, CPU
scheduling, I/O), then measure the latency distribution.

**Results:** see [`docs/matching-engine-project-doc.md`](docs/matching-engine-project-doc.md)
for the full report and [`results/summary.md`](results/summary.md) for the generated
tables. Headline, at 200 k orders/s on this host: the two queues have the same
median latency (~25 µs, p = 0.14), but the SPSC pipeline has **half the p99**
(37.6 vs 70.9 µs, p = 0.001) and **one third the matching-side p99.9**
(20.7 vs 68.6 µs), sustains **5.86 M/s** unthrottled against 3.34 M/s, and absorbs
the client's maximum offered rate (6.40 M/s) with zero full-queue waits while the
mutex path collapses to 3.3 M/s in 2 of 3 runs at that rate. The matching step
itself is ~94 ns; 86–88 % of the median is TCP ingress and wake-up.

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

The whole measurement chain (environment capture, Release rebuild, five phases, both
analyses, thermal sampling, ~16 minutes) is one command:

```bash
./scripts/run_session.sh
```

It refuses to start on a dirty tree, sets the platform profile to `performance`,
preserves the previous session's data, and writes a transcript to
`results/bench-session-<ts>.log`. Run it from a TTY with no graphical session
active and leave the machine alone while it runs; that is the condition the
reported numbers were collected under.

Individual phases:

```bash
scripts/env.sh                 # results/env.txt (host, power profile, EPP, thermal state)
scripts/run_matrix.sh          # 2 queues × 2 scheduling × 2 networks, 10 latency + 5 throughput runs each
scripts/run_experiments.sh     # allocator variants (slab/heap/V1) + SCHED_FIFO probe
scripts/run_saturation.sh      # 2 queues × 9 offered rates (50k … 6.4M/s, unthrottled)
scripts/run_components.sh      # no-network floors + allocator isolation + false sharing
scripts/run_probe.sh           # ingress attribution + client/network pinning probes
scripts/analyze.py             # results/summary.csv, summary.md, analysis.json, plots/*.svg
scripts/analyze_components.py  # results/components-summary.md
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
| `ME_SAT_REPS` | `3` | repetitions per saturation rate |
| `ME_COMPONENT_REPS` | `9` | repetitions per component benchmark |
| `ME_PROBE_REPS` | `3` | repetitions per probe |
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

## Measurement conditions and caveats

The reported numbers were collected on the host described in `results/env.txt`: Intel
i5-12450HX, kernel 7.2.9-arch1-1, GCC 16.2.1, `platform_profile=performance`
(`EPP=performance` on all CPUs), on AC, from a TTY with no graphical session,
`loadavg` 0.21 at start. Package temperature stayed at 47–71 °C with the P-cores at
4.37–4.42 GHz, so no run was thermally throttled.

Limitations that matter when reading the numbers:

- The CPU governor is still `powersave` and turbo is enabled; the benchmark cannot
  change them without root. Clocks varied 0.8–4.4 GHz between runs, so ratios are
  more portable than absolute values.
- `SCHED_FIFO` is denied to the benchmark user (`rtprio_limit=0`, no `CAP_SYS_NICE`).
  The RT configuration is reported as blocked, not fabricated.
- `perf` and `strace` are unavailable (`perf_event_paranoid=2`), so mechanism
  evidence comes from in-process counters: `getrusage(RUSAGE_THREAD)` for context
  switches and CPU time, `sched_getcpu()` for migrations, and per-backend syscall /
  submission / completion counters.
- SMT is enabled and `isolcpus` is not used, so a pinned thread still shares its core
  with whatever else the host schedules there. Pinning the engine cut its run-queue
  wait from 2.20 ms to 0.22 ms but improved p99.9 only from 127.7 to 86.8 µs, which
  is not statistically significant at n = 10 (p = 0.14).
- The network thread is left floating by default. The probe that pins it to one core
  raised p99.9 from 51.5 to 127.5 µs; a confined thread cannot migrate away from
  unrelated work.
- All networking is loopback. The measurements quantify kernel/user-boundary and
  scheduling effects; they are not a NIC or cross-host benchmark.
- `results/summary.md` separates *clean* runs from host-contended runs. A run is
  contended when at least one order was already >1 ms old at parse time; those runs
  are listed individually rather than deleted, and configurations are ranked on clean
  runs only. `results/host-isolation.txt` records a controlled check — the same
  configuration, same binary, moving from 2.40 ms to 142 µs p99.9 when an unrelated
  desktop process was confined to the E-cores — which is what justifies treating the
  outliers as ingress delay rather than pipeline behaviour.

## Documentation

| Path | Contents |
|---|---|
| `docs/matching-engine-project-doc.md` | final report: design, methodology, results, conclusions |
| `docs/contracts/phase-1-component-contract.md` | interface contract for the phase-1 components |
| `docs/decisions/ring-buffer.md` | why the SPSC ring buffer is built the way it is |
| `docs/iterations/0-iteration-plan.md` | the original iteration plan |
| `docs/init/` | the submitted proposal and team plan (historical) |
| `results/summary.md` | generated integrated tables (latency, throughput, saturation) |
| `results/components-summary.md` | generated component benchmarks and their checks |
| `results/env.txt` | host, power profile, per-CPU EPP/frequency, thermal snapshot, commit |

## Layout

```
include/      headers: engine, books, queues, wire format, recency, affinity
src/          matching engine app, network backends, V1 book, mutex queue
tools/        load_client
tests/        unit, differential, parser and concurrency tests
bench/        standalone book, pipeline, allocator and false-sharing microbenchmarks
scripts/      environment capture, run harness, analysis, verification
results/      env.txt, summary.csv/md, components, plots, raw per-run CSVs
docs/         final report, contracts, decisions, iteration plan, original proposal
```
