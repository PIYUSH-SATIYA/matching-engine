# Matching Engine: Project and Benchmark Guide

This document explains how to set up, build, run, test, benchmark, and inspect
the Linux C++20 matching-engine project.

The engine consists of a TCP network thread, a mutex or lock-free SPSC queue, a
single-threaded price-time-priority matching engine, interchangeable order books,
and `epoll`/`io_uring` network backends. The current 40-byte wire protocol sends
new orders only; it does not represent cancels, modifications, market orders, or
IOC/FOK orders.

## 1. Repository layout

```text
src/                         server and matching-engine implementation
include/                     C++ headers and 40-byte wire protocol
tools/load_client.cpp        original reproducible client
tools/workload_client.cpp    profile-driven realistic-load client
bench/                       no-network component benchmarks
tests/                       unit and differential tests
scripts/                     build, run, benchmark, and analysis scripts
results/                     generated benchmark data and reports
docs/                        reports and guides
```

Start from a clean checkout when possible:

```bash
git clone <repository-url>
cd matching-engine
git status --short
git log -1 --oneline
```

`run_session.sh` intentionally refuses to benchmark a dirty tree.

## 2. Native setup and Release build

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake ninja gcc pkgconf liburing python
```

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config liburing-dev python3
```

The analysis scripts use only Python's standard library.

### Build and test

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
ctest --test-dir build-rel --output-on-failure
```

The build produces:

```text
build-rel/matching_engine
build-rel/load_client
build-rel/workload_client
build-rel/bench_order_book
build-rel/bench_pipeline
build-rel/bench_allocator
build-rel/bench_false_sharing
```

CMake enables `io_uring` only when `liburing` is found. `epoll` remains available
without it.

### Sanitizers

ASan/UBSan and TSan are separate builds:

```bash
cmake -S . -B build-asan -DENABLE_SANITIZERS=ON
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan -j"$(nproc)"
ctest --test-dir build-tsan -R 'ring_buffer|spsc' --output-on-failure
```

Do not use sanitizer binaries for performance measurements. `scripts/verify.sh`
automates the Release, sanitizer, and epoll/io_uring checksum checks:

```bash
scripts/verify.sh
```

## 3. Container workflow

The container files are:

| File | Purpose |
|---|---|
| `Dockerfile` | Ubuntu 24.04 image with GCC, CMake, Ninja, GDB, Python, and liburing |
| `docker-compose.yml` | development container with the repository mounted at `/workspace` |
| `dev.sh` | wrapper for `docker compose run --rm dev` |

Build and test through Docker:

```bash
./dev.sh cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
./dev.sh cmake --build build-rel -j"$(nproc)"
./dev.sh ctest --test-dir build-rel --output-on-failure
```

For an interactive container:

```bash
./dev.sh bash
```

The repository is bind-mounted. Therefore a C++ edit made on the host is visible
inside the container. After changing C++:

```bash
./dev.sh cmake --build build-rel -j"$(nproc)"
./dev.sh ctest --test-dir build-rel --output-on-failure
```

You do not need to rebuild the image for source changes. Rebuild only after
changing `Dockerfile` or system packages:

```bash
docker compose build dev
```

Build directories and `results/` are visible on the host. The container runs as
root by default, so newly created build files may be root-owned. A container also
does not isolate the host from CPU contention; TTY benchmarks still need a quiet
machine.

To run both processes inside one container shell:

```bash
./dev.sh bash
```

Then inside the shell:

```bash
./build-rel/matching_engine --port 9000 --queue spsc --book fast-slab \
  --net epoll --sched default --warmup 10000 \
  --stats results/raw/container-demo/runs.csv --config container-demo --run 1 \
  --kind latency --exit-on-disconnect >results/raw/container-demo/server.log 2>&1 &

taskset -c 6 ./build-rel/load_client 127.0.0.1 9000 1010000 42 200000
```

## 4. Incremental C++ development loop

After changing a `.cpp` or header:

```bash
cmake --build build-rel -j"$(nproc)"
ctest --test-dir build-rel --output-on-failure
```

After changing CMake files:

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
```

If the CMake cache points at another source directory, use a fresh build directory:

```bash
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release
cmake --build build-local -j"$(nproc)"
```

Useful focused tests:

```bash
ctest --test-dir build-rel -R 'order_book|matching_engine|wire' --output-on-failure
ctest --test-dir build-rel -R 'ring_buffer|spsc' --output-on-failure
```

Do not use sanitizer binaries for performance measurements.

## 5. Manual server and original client

Start the server first:

```bash
./build-rel/matching_engine \
  --port 9000 \
  --queue spsc \
  --book fast-slab \
  --net epoll \
  --sched pin \
  --cpu-engine 2 \
  --warmup 10000 \
  --stats results/raw/demo/runs.csv \
  --config demo \
  --run 1 \
  --kind latency \
  --offered-rate 200000 \
  --exit-on-disconnect
```

Then, in another terminal:

```bash
taskset -c 6 ./build-rel/load_client 127.0.0.1 9000 1010000 42 200000
```

The original client syntax is:

```text
load_client HOST PORT N [SEED] [RATE]
```

`N` includes warm-up messages. `RATE=0` is unthrottled. Positive rates use an
absolute schedule and 64-order batches; unthrottled mode uses larger batches.

## 6. Server flags

Run `./build-rel/matching_engine --help` for the live list.

| Flag | Values/default | Meaning |
|---|---|---|
| `--port` | integer, `9000` | TCP listening port |
| `--queue` | `mutex`/`spsc`, default `spsc` | synchronization mechanism |
| `--book` | `v1`/`fast-slab`/`fast-heap` | order-book implementation |
| `--net` | `epoll`/`uring` | network backend |
| `--sched` | `default`/`pin`/`rt` | scheduling mode |
| `--cpu-engine` | CPU number | engine affinity for `pin`/`rt` |
| `--cpu-net` | CPU number | optional network-thread affinity |
| `--warmup` | count | discarded latency samples |
| `--samples` | path | raw binary latency samples |
| `--stats` | path | append one CSV row |
| `--config` | label | CSV configuration label |
| `--run` | integer | CSV repetition number |
| `--kind` | label | `latency`, `throughput`, `saturation`, or `workload` |
| `--offered-rate` | number | recorded metadata; server does not pace the client |
| `--count-migrations` | flag | sample CPU migrations; adds overhead |
| `--exit-on-disconnect` | flag | stop after the final client disconnects |
| `--log` | path | write a trade log |

`pin` is not CPU isolation. A pinned engine may still share its CPU with other
work. The network thread floats unless `--cpu-net` is supplied. `rt` may fail with
`EPERM` without realtime privileges.

## 7. Original benchmark suite

Run the complete experiment from a quiet TTY:

```bash
./scripts/run_session.sh
```

It captures the environment, builds Release, runs the matrices, analyzes results,
and records thermal data. It writes a transcript at
`results/bench-session-<timestamp>.log` and refuses a dirty tree.

Individual phases:

```bash
scripts/env.sh                 # host, power, EPP, thermal metadata
scripts/run_matrix.sh          # queues × scheduling × network
scripts/run_experiments.sh     # allocator and realtime probes
scripts/run_saturation.sh      # offered-rate sweep
scripts/run_components.sh      # no-network floors and allocator tests
scripts/run_probe.sh           # ingress and affinity probes
scripts/analyze.py             # main summary and SVG plots
scripts/analyze_components.py  # component summary
```

Important environment variables:

| Variable | Default | Meaning |
|---|---:|---|
| `ME_PORT` | `9000` | server port |
| `ME_WARMUP` | `10000` | warm-up count |
| `ME_LAT_RUNS` | `10` | latency repetitions |
| `ME_LAT_ORDERS` | `1010000` | latency orders |
| `ME_LAT_RATE` | `200000` | latency offered rate |
| `ME_THR_RUNS` | `5` | throughput repetitions |
| `ME_THR_ORDERS` | `2000000` | throughput orders |
| `ME_SAT_REPS` | `3` | saturation repetitions |
| `ME_CPU_ENGINE` | `2` | engine CPU |
| `ME_CPU_NET` | `4` | optional network CPU |
| `ME_CPU_CLIENT` | `6` | client CPU |
| `ME_PIN_NET` | `0` | set to `1` to pin network thread |
| `ME_SEED` | `42` | workload seed |
| `BIN` | `build-rel` | build directory used by scripts |
| `RESULTS` | `results` | script output root |

## 8. Profile-driven realistic workloads

Build the separate client:

```bash
cmake --build build-rel -j"$(nproc)" --target workload_client
```

Run fixed-rate realistic traffic:

```bash
scripts/run_workload.sh --profile market-like --runs 5 \
  --orders 1010000 --rate 200000
```

Run unthrottled throughput traffic:

```bash
scripts/run_workload.sh --profile market-like --runs 5 \
  --orders 2000000 --rate 0
```

Run invalid and duplicate traffic:

```bash
scripts/run_workload.sh --profile invalid-mix --runs 5 \
  --orders 2000000 --rate 200000 \
  --invalid-rate 0.01 --duplicate-rate 0.01
```

Profiles:

| Profile | Behaviour |
|---|---|
| `uniform` | simple compatibility workload |
| `clustered-book` | prices concentrated near liquidity clusters |
| `market-like` | side imbalance, clustered prices, lognormal quantities, changing regimes |
| `burst` | quiet intervals and high-rate bursts |
| `regime-switch` | calm, buy-heavy, volatile, and sell-heavy phases |
| `invalid-mix` | market-like traffic with invalid fields and duplicate IDs |

Workload flags:

| Flag | Meaning |
|---|---|
| `--profile NAME` | generator profile |
| `--runs N` | repetitions |
| `--orders N` | messages per repetition |
| `--rate R` | offered rate; `0` means unthrottled |
| `--seed N` | deterministic base seed |
| `--invalid-rate F` | fraction with invalid ID, side, or quantity |
| `--duplicate-rate F` | fraction with duplicate IDs |
| `--buy-probability F` | buy-side probability override |
| `--queue`, `--book`, `--net`, `--sched` | server choices forwarded by wrapper |
| `--force` | reuse an existing timestamped output directory |

Compare existing profile runs:

```bash
python3 scripts/analyze_workload.py --input results/workloads --recursive
```

Keep fixed-rate latency and unthrottled throughput in separate reports:

```bash
python3 scripts/analyze_workload.py --input results/workloads --recursive \
  --offered-rate 0 --output results/workloads/unthrottled-report

python3 scripts/analyze_workload.py --input results/workloads --recursive \
  --offered-rate 200000 --output results/workloads/latency-report
```

## 9. Results and interpretation

Original benchmark outputs:

```text
results/raw/<configuration>/runs.csv
results/summary.csv
results/summary.md
results/analysis.json
results/plots/*.svg
results/components-summary.md
results/env.txt
results/host-isolation.txt
```

Realistic workload outputs:

```text
results/workloads/<profile>/<timestamp>/
```

Each run contains `manifest.json`, `client-summary.csv`, `client.log`,
`server.log`, `runs.csv`, `run-config.txt`, `summary.md`, `summary.csv`,
`analysis.json`, and `plots/*.svg`.

The server CSV contains latency, throughput, accepted/rejected/invalid counts,
trades, queue waits, arrival percentiles, service percentiles, and scheduler
run-queue counters. `invalid` means the wire parser rejected a record before it
entered the queue. `invalid_generated` and `duplicate_generated` are client-side
generation counters. `rejected` means a valid order reached the book but was
refused, for example because of a duplicate active ID, unsupported price, or
capacity limit.

Do not compare p50 from an unthrottled run with p50 from a 200k/s open-loop run.
Unthrottled traffic measures capacity and can create backlog; fixed-rate traffic
measures latency at a controlled offered load. For capacity use throughput and
queue waits. For latency use p50/p95/p99/p99.9 across multiple repetitions.

## 10. Troubleshooting

### CMake cache points elsewhere

Use a fresh build directory:

```bash
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release
cmake --build build-local -j"$(nproc)"
```

Do not reuse a cache generated in `/workspace` for a native build.

### `io_uring` is unavailable

Install `liburing`/`liburing-dev` and reconfigure. Check CMake output for
`io_uring backend enabled`.

### `taskset` fails

```bash
nproc
taskset -pc $$
```

Use valid CPUs or disable client pinning with `ME_PIN_CLIENT=0`.

### Server does not exit

Use `--exit-on-disconnect`, check that the client completed, and inspect the
matching `server.log` and `client.log`.

### Good throughput but huge latency

This normally means unthrottled backlog formation. Use a positive `--rate` for
latency experiments and `--rate 0` only for capacity experiments.

## 11. UI/UX direction

This is currently a local systems benchmark, not a web application. Markdown
reports and SVG charts are the correct reproducible interface. A low-risk UX layer
can be added without touching the timed engine:

1. Add a terminal status command listing completed profiles, rates, seeds, and
   missing artifacts.
2. Add a static `results/index.html` linking summaries, CSV files, logs, and SVGs.
3. Add comparison cards for p50/p99/p99.9, throughput, invalid, rejected, and
   queue-wait differences.
4. Warn when a user compares different offered rates or host-contended runs.
5. Add a manifest schema/version for future workload generators.

Do not put a live dashboard in the benchmark process: polling or rendering during
the run can perturb scheduler and CPU measurements. A static viewer reading
`analysis.json` after the run is safer. A full frontend framework, database, or
remote service is unnecessary until the project needs multi-user result storage.

## 12. Quick reference

```bash
# Native build and tests
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
ctest --test-dir build-rel --output-on-failure

# Container build and tests
./dev.sh cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
./dev.sh cmake --build build-rel -j"$(nproc)"
./dev.sh ctest --test-dir build-rel --output-on-failure

# Original complete experiment
./scripts/run_session.sh

# Fixed-rate realistic workload
scripts/run_workload.sh --profile market-like --runs 5 --rate 200000

# Unthrottled capacity workload
scripts/run_workload.sh --profile market-like --runs 5 --rate 0 --orders 2000000

# Invalid/duplicate workload
scripts/run_workload.sh --profile invalid-mix --runs 5 --rate 200000 \
  --invalid-rate 0.01 --duplicate-rate 0.01
```
