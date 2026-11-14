# Final benchmark report — low-latency matching engine

Status: measurement pass complete on `agent/final-benchmark-suite`, commit
`9f1f94a` (artifacts committed in `cceb095`). This document reports **only what
the recorded runs show**. It is deliberately separate from the design report;
nothing in the older docs was edited.

All raw data, generated tables and plots live under `results/` and can be
regenerated from the scripts in `scripts/`.

---

## 1. What was measured

| Block | Shape | Runs |
|---|---|---|
| Integrated matrix | queue {mutex, spsc} × scheduling {default, pin} × network {epoll, io_uring} | 8 configs × 10 latency + 5 throughput |
| Allocator in the pipeline | fast-slab / fast-heap / V1 book | 3 × 10 latency + 5 throughput |
| Saturation sweep | queue {spsc, mutex} × offered rate {50k … 6.4M/s, unthrottled} | 2 × 9 × 3 |
| Components | matching core, queue+core, allocator isolation, false sharing | 4 × 9 reps |
| Probes | ingress attribution, client pinning, net pinning | 4 × 3 |

3,010,000 orders were sent in each 6.4 M/s saturation run; 2,000,000 in each
unthrottled run; 1,010,000 in each latency run.

### Workload and protocol

- One TCP client, deterministic seed 42, 64-byte batches when paced.
- 10,000 orders of warm-up discarded before any sample is recorded.
- Latency runs: open loop at 200,000 orders/s, 10 repetitions.
- Throughput runs: unthrottled, 5 repetitions.
- Reported values are **medians across repetitions**, not means. The p99.9
  range across repetitions is printed so the spread is visible.
- Latency is client-send → engine-complete, measured with
  `CLOCK_MONOTONIC` on the same host. Both timestamps are taken inside the
  process; no network time synchronisation is involved.

### Instrumentation definitions

These matter for reading the tables:

| Column | Meaning |
|---|---|
| `arrival` | client timestamp → network thread finished parsing |
| `service` | parse → matching engine finished the order |
| `total` | arrival + service |
| `net_ingest_slow` | orders already older than 1 ms when parsed |
| `engine_runq_ns` / `net_runq_ns` | time the thread spent runnable but not on a CPU (`schedstat`) |
| `nvcsw` / `nivcsw` | voluntary / involuntary context switches (`getrusage`) |
| `kernel_ops` | epoll: waits + recv calls + accepts; io_uring: submits + completions + accepts |
| `queue_full_waits` | blocking `push` calls that found no room |
| `queue_empty_waits` | blocking `pop` calls that found nothing |
| **clean run** | no order was already >1 ms old at parse |

A "clean run" is the measurement-quality gate. When the desktop delayed
ingress, that is recorded rather than deleted. Configs with zero clean runs
are marked `–` and must not be ranked by tail latency.

---

## 2. Environment and blocked controls

| Item | Value |
|---|---|
| Kernel | `7.2.9-arch1-1`, `PREEMPT_DYNAMIC` |
| CPU | 4 P-cores (2 SMT threads each, ≤4.4 GHz) + 4 E-cores (≤3.1 GHz), 12 threads |
| Compiler | GCC 16.2.1, Release (`-O3 -DNDEBUG`), no sanitizers |
| liburing | 2.15 |
| Governor | `powersave` on all CPUs |
| Turbo | enabled (`intel_pstate/no_turbo=0`) |
| SMT | enabled |
| `SCHED_FIFO` | **denied** (`EPERM`, `rtprio_limit=0`) |
| `perf` | **unavailable** (binary missing, `perf_event_paranoid=2`) |
| `strace` | unavailable |
| Host | desktop session, browser/terminal load present during the run |

So the matrix is a *fair A/B under identical conditions*, not an absolute
latency floor. Frequency scaling, SMT sharing and host scheduling all add
noise that could be removed only with root.

---

## 3. Component results

### 3.1 Matching core (single thread, no queue, no network)

`results/components/bench-order-book.txt`, 2M operations, 9 reps.

| book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| V1 (`map` + `list` + `unordered_map`) | 9.66 M/s | 98 ns | 483 ns | 689 ns | **13.8 ms** |
| V3 (array + slab + bitmap) | **29.18 M/s** | 107 ns | 272 ns | 438 ns | **25.8 µs** |

Read this correctly: V3 is not faster per operation at the median. It removes
the *tail*. The 13.8 ms V1 spike is a red-black-tree rebalance / allocator
event; V3's worst case over the same workload is 25.8 µs. Both produce
identical trade counts and notional.

### 3.2 Allocator in isolation

`results/components/bench-allocator.txt`, 2M operations on 64-byte nodes,
live set 8192, 70 % allocate / 30 % release, 9 reps.

| allocator | alloc ns/op | alloc p99.9 | alloc sd | free ns/op | free p99.9 | free sd |
|---|---|---|---|---|---|---|
| aligned `::operator new/delete` | 19.77 | 49 ns | 21.5 | 25.03 | 110 ns | 19.7 |
| `SlabAllocator` | **14.56** | **19 ns** | **14.7** | **16.41** | **29 ns** | **13.5** |

Speed-up 1.36× allocate, 1.53× release; variance roughly halved. Absolute
values include ~20–25 ns of clock cost in both variants, so the ratio and the
spread are the meaningful numbers, not the absolute ns.

### 3.3 Queue + core, no network

`results/components/bench-pipeline.txt`, 1.01M orders paced at 200k/s, 9 reps.

| queue + book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| spsc + V3 | **6.06 M/s** | 335 ns | 561 ns | **1.1 µs** | 29.4 µs |
| spsc + V1 | 4.89 M/s | 357 ns | 922 ns | 8.17 ms | 12.9 ms |
| mutex + V3 | 2.92 M/s | 1.8 µs | 2.3 µs | 6.7 µs | 910 µs |
| mutex + V1 | 2.75 M/s | 1.8 µs | 2.6 µs | 7.69 ms | 12.5 ms |

This isolates the queue hand-off: no sockets, no wire parsing, no kernel
transitions. The queue difference is one order of magnitude in throughput and
~5× in median latency; the book difference is one order of magnitude in the
tail.

### 3.4 False sharing (padded vs packed SPSC indices)

`results/components/bench-false-sharing.txt`, identical push/pop logic,
5M orders per rep, 9 reps.

| layout | throughput median | throughput best | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| padded indices | 6.95 M/s | 18.11 M/s | 183 ns | 276 ns | 5.0 µs |
| packed indices | 8.80 M/s | 16.94 M/s | 258 ns | 5.8 µs | 30.4 µs |

**This is not a clean win for padding, and that is the honest finding.**
Throughput on this host is bimodal (~6.9 M/s and ~13–18 M/s) for *both*
layouts — preemption of a spin-waiting pair only subtracts throughput, and it
hit both variants. Best-case is 18.11 vs 16.94 M/s (1.07×), i.e.
indistinguishable. The reproducible difference is in the tail: packed p99 is
~20× worse and p99.9 ~6× worse. Cache-line invalidations land on the
empty/full refresh path, so they add latency spikes rather than reducing
sustained rate. Treat the tail result as *suggestive*; it is not a controlled
result on a noisy desktop.

---

## 4. Integrated pipeline

### 4.1 Throughput (unthrottled, 5 runs, medians)

| config | orders/s | engine CPU/order | net CPU/order | net `nvcsw` | kernel ops/run |
|---|---|---|---|---|---|
| spsc / default / epoll | **5.00 M/s** | 0.20 µs | 0.20 µs | 14 | 20,182 |
| spsc / default / uring | 4.88 M/s | 0.20 µs | 0.20 µs | 11 | 40,328 |
| spsc / pin / uring | 4.75 M/s | 0.21 µs | 0.21 µs | 12 | 40,328 |
| spsc / pin / epoll | 4.72 M/s | 0.21 µs | 0.21 µs | 11 | 20,178 |
| mutex / pin / epoll | 2.99 M/s | 0.34 µs | 0.24 µs | **169,434** | 20,170 |
| mutex / default / uring | 2.96 M/s | 0.34 µs | 0.24 µs | 156,991 | 40,328 |
| mutex / pin / uring | 2.96 M/s | 0.34 µs | 0.23 µs | 164,515 | 40,328 |
| mutex / default / epoll | 2.88 M/s | 0.35 µs | 0.24 µs | 164,688 | 20,168 |

**Important ceiling caveat.** These "unthrottled" numbers are
*client-limited*. The unthrottled client sends 1024-order batches, blocks on a
full socket, and delivered 5.06M orders/s in the 2M-order runs. The paced
6.4 M/s client sends 64-order batches, never blocks, and delivered 6.40M
orders/s, which the spsc pipeline fully consumed. The server ceiling is
therefore **at least 6.4M/s**, not 5.0M/s. The mutex number is a genuine
service ceiling; the spsc number is a measurement-harness ceiling.

The mechanism behind the spsc/mutex gap is directly measured, not inferred:
the network thread made **157k–169k voluntary context switches per run**
while pushing into the mutex queue (it blocks inside `push`) versus **7–14**
for spsc, and the engine burned 0.34 vs 0.20 µs of CPU per order. The engine's
own involuntary switches are small in both cases (73–129); the cost is the
producer's block/wake cycle, not engine preemption.

### 4.2 Latency at 200k orders/s (medians of 10 runs)

| config | p50 | p95 | p99 | p99.9 | clean runs | p99.9 clean |
|---|---|---|---|---|---|---|
| spsc / default / epoll | 24.4 µs | 49.2 µs | 141.6 µs | 1.34 ms | 1/10 | 142.7 µs |
| spsc / default / uring | 24.5 µs | 46.1 µs | 98.3 µs | 151.6 µs | 8/10 | 149.9 µs |
| spsc / pin / epoll | **22.6 µs** | 46.8 µs | 174.7 µs | 1.79 ms | 1/10 | 159.3 µs |
| spsc / pin / uring | 23.3 µs | 64.4 µs | 716.1 µs | 2.78 ms | 0/10 | – |
| mutex / default / epoll | 26.2 µs | 62.4 µs | **94.9 µs** | **170.2 µs** | **10/10** | 170.2 µs |
| mutex / default / uring | 26.9 µs | 66.9 µs | 118.3 µs | 563.2 µs | 4/10 | 171.0 µs |
| mutex / pin / epoll | 31.8 µs | 132.1 µs | 1.63 ms | 2.84 ms | 0/10 | – |
| mutex / pin / uring | 33.0 µs | 73.1 µs | 272.4 µs | 1.21 ms | 7/10 | 1.13 ms |

At this offered rate the two queues are **comparable at the median** (24.4 vs
26.2 µs for the default/epoll pair) and in the clean tail (150–159 vs 170 µs).
The spsc advantage is throughput and CPU efficiency, not latency at 200k/s.
This is a useful negative result: the queue design is not what limits latency
at moderate load.

### 4.3 Latency decomposition (medians, same runs)

Total p99.9 = arrival p99.9 + service p99.9 (approximately; percentile sums
are not exact).

| config | total p99.9 | arrival p99.9 | service p99.9 | arrival share |
|---|---|---|---|---|
| spsc / default / epoll | 1.34 ms | 1.20 ms | 98.0 µs | 90 % |
| spsc / default / uring | 151.6 µs | 129.2 µs | 29.8 µs | 85 % |
| spsc / pin / epoll | 1.79 ms | 1.75 ms | 92.2 µs | 98 % |
| spsc / pin / uring | 2.78 ms | 2.75 ms | 92.0 µs | 99 % |
| mutex / default / epoll | 170.2 µs | 131.3 µs | 126.3 µs | 77 % |
| mutex / pin / epoll | 2.84 ms | 1.55 ms | 2.81 ms | 55 % |
| V1 book (spsc) | 9.56 ms | 124.6 µs | 9.54 ms | 1 % |

In every spsc configuration, **77–99 % of the tail is the order waiting to be
parsed**, not matching. The V1 row is the exception: there the book itself
dominates, which is exactly what the component benchmark predicts.

### 4.4 Allocator (fixed: spsc, epoll, pinned)

| config | p50 | p95 | p99 | p99.9 | clean runs |
|---|---|---|---|---|---|
| allocator: fast-slab | 25.6 µs | 38.9 µs | 53.3 µs | 155.9 µs | 10/10 |
| allocator: fast-heap | 26.2 µs | 40.1 µs | 52.8 µs | 152.2 µs | 9/10 |
| allocator: V1 book | 24.2 µs | 40.2 µs | 84.0 µs | 9.56 ms | 10/10 |

The isolated allocator is 1.36–1.53× faster, but **the difference is
invisible end-to-end at 200k/s** (152.2 vs 155.9 µs p99.9, inside run-to-run
noise). At this rate the allocation is off the critical path. The V1 row is
the control that shows what a pathological allocator *does* do: 9.56 ms p99.9
and 84 µs p99.

### 4.5 Scheduling and affinity

| fixed spsc / fast-slab / epoll | clean runs | p50 | p99 | p99.9 | engine run-queue wait | involuntary switches |
|---|---|---|---|---|---|---|
| default scheduler | 1/10 | 24.4 µs | 141.6 µs | 1.34 ms | 10.7 ms | 464 |
| engine pinned | 1/10 | 22.6 µs | 174.7 µs | 1.79 ms | **0.2 ms** | **33** |

Pinning works exactly where it is designed to work: engine run-queue wait
drops 50× and involuntary switches drop 14×. It does **not** improve the
end-to-end tail on this host, because the network thread is left floating and
is the component that gets delayed (see §6). Pinning one thread of a two-thread
pipeline is not enough.

### 4.6 epoll vs io_uring

Kernel operations per run, medians:

| regime | epoll | io_uring |
|---|---|---|
| paced 200k/s | 46,606–47,340 | 31,312–31,561 |
| unthrottled | 20,168–20,182 | 40,328 |

The inversion is a property of the code paths, not noise: at 200k/s epoll
calls `epoll_wait` + `recv` per wake-up, while io_uring submits one multi-shot
receive and gets many completions; at saturation epoll's drain loop amortises
one wait over many `recv`s, while io_uring still pays one submit per
receive (`kBufSize = 4096`). Latency differences between the two are not
consistent across the matrix and cannot be claimed from this host.

### 4.7 CPU accounting

| queue | engine CPU, 200k/s | engine CPU, unthrottled | CPU/order |
|---|---|---|---|
| spsc | ~101 % (spins) | ~100 % | 0.20 µs |
| mutex | 8–9 % (sleeps on condvar) | 100 % | 0.34 µs |

This is the clearest OS-level trade-off in the project: the lock-free queue
buys latency and throughput by burning a core; the mutex baseline is
power-friendly and equally fast at the median, but collapses under load.

---

## 5. Saturation (proposal Experiment 5)

Offered rate is what the client paced; achieved is what the server completed.

| offered | spsc achieved | spsc p50 | spsc full waits | mutex achieved | mutex p50 | mutex full waits |
|---|---|---|---|---|---|---|
| 50 k/s | 50 k/s | 93.1 µs | 0 | 50 k/s | 240.0 µs | 0 |
| 100 k/s | 100 k/s | 37.6 µs | 0 | 100 k/s | 69.4 µs | 0 |
| 200 k/s | 200 k/s | 24.7 µs | 0 | 200 k/s | 31.9 µs | 0 |
| 400 k/s | 400 k/s | 15.5 µs | 0 | 400 k/s | 17.6 µs | 0 |
| 800 k/s | 800 k/s | 13.7 µs | 0 | 800 k/s | 18.2 µs | 0 |
| 1.6 M/s | 1.6 M/s | 14.1 µs | 0 | 1.6 M/s | 15.6 µs | 0 |
| 3.2 M/s | 3.2 M/s | 12.2 µs | 0 | 3.2 M/s | 15.3 µs | 0 |
| 6.4 M/s | **6.4 M/s** | 12.6 µs | **0** | **2.88 M/s** | **25.5 ms** | **383,834** |
| unthrottled | 4.74 M/s | 15.3 ms | 1,842,771 | 2.91 M/s | 24.6 ms | 249,188 |

Three findings:

1. **The mutex queue congestion-collapses** when offered load exceeds its
   ~2.9 M/s service ceiling: median latency jumps from 15 µs to 25 ms and
   ~384k pushes block on the condition variable. This is the quantitative
   form of "the mutex becomes the bottleneck."
2. **The SPSC queue keeps up at 6.4 M/s with zero full waits.** The knee is
   above 6.4 M/s; the harness cannot drive it higher.
3. **Latency is not monotonic in offered rate.** At 50k/s the mutex median is
   240 µs, worse than at 3.2M/s (15.3 µs), because the engine sleeps and pays
   wake-up latency. The sweet spot is roughly 400k–3.2M/s. Below it the price
   is idle wake-up; above it, queueing.

Caveat for the unthrottled rows: end-to-end latency there includes kernel
socket buffers and client send blocking, not just the ring buffer. Those rows
measure *pipeline response time*, not queue occupancy time.

---

## 6. Host-noise attribution (why the multi-millisecond tails exist)

`results/host-isolation.txt` records a one-off controlled experiment: the
desktop process responsible for sustained load was confined to the E-cores
without being killed, and the same spsc/pinned configuration was measured
before and after.

| condition | n | p50 | p99 | p99.9 | max | net run-queue wait | orders >1 ms at parse |
|---|---|---|---|---|---|---|---|
| desktop free to use all CPUs | 3 | 26.0 µs | 1,046 µs | 2,400 µs | 2,896 µs | 97.7 ms | 10,017 |
| desktop confined to E-cores | 3 | 22.5 µs | 68 µs | **142 µs** | 240 µs | 1.2 ms | **0** |

The engine thread's run-queue wait in the confined half was 0.29 ms, so the
engine was not the delayed component — the network thread was. A mutex control
in the same window gave p99.9 = 299 µs, consistent with its uncontended rows.

**Conclusion: the 1–3 ms tails in the matrix are host ingress scheduling, not
matching latency.** This is corroborated independently by the in-process
`net_ingest_slow`, arrival/service split, and `net_runq` columns (§4.3).

---

## 7. Correctness and parity evidence

- 7/7 unit/integration tests pass in Release and under ASan/UBSan.
- 2/2 ring-buffer tests pass under TSan.
- epoll and io_uring produce identical trade count, quantity and notional in
  `verify.sh`.
- Identically seeded matrix runs produce **one** checksum group for latency
  (788,580 trades, qty 20,127,113, notional 201,270,962,137) and **one** for
  throughput (1,561,056 trades, qty 39,843,705, notional 398,436,546,776)
  across all queue, allocator, scheduling and network variants.
- Component benches reproduce the same checksum across all four queue/book
  combinations.

Correctness is therefore not conditional on the performance numbers.

---

## 8. What we can honestly claim

### Demonstrated (direct, reproducible measurement)

1. The SPSC queue sustains **≥1.6× the mutex throughput** (5.00 vs 2.99 M/s
   measured, ≥6.4 M/s demonstrated when paced) at **0.20 vs 0.34 µs** engine
   CPU per order, with **7–14 vs 157k–169k** voluntary context switches in the
   producer (blocking `push`) per run.
2. The mutex queue **collapses above ~2.9 M/s** offered: 25 ms median, 384k
   blocking pushes. The SPSC queue absorbs 6.4 M/s with zero full waits.
3. At 200k orders/s both queues have **comparable median latency** (24.4 vs
   26.2 µs for default/epoll) and comparable clean p99.9 (~150–170 µs). The
   queue choice is a throughput/CPU trade, not a latency win at moderate load.
4. **77–99 % of the spsc tail is ingress delay**, not matching. Confining the
   competing desktop process moves p99.9 from 2.4 ms to 142 µs and eliminates
   all >1 ms arrivals.
5. The V3 book is **3.0× faster** than V1 (29.18 vs 9.66 M/s) and removes a
   **13.8 ms → 25.8 µs** worst case. In the integrated pipeline the V1 book
   alone produces 9.56 ms p99.9.
6. The slab allocator is **1.36× faster to allocate, 1.53× faster to
   release**, with roughly half the standard deviation and p99.9 19 vs 49 ns
   in isolation. It is **not measurable end-to-end** at 200k/s.
7. Engine pinning cuts the engine's run-queue wait **10.7 ms → 0.2 ms** and
   involuntary switches **464 → 33**, but does not improve end-to-end tails
   while the network thread floats on a busy host.
8. All configurations are numerically identical (checksums above) and pass
   tests/sanitizers.

### Supported, but host-limited

9. epoll and io_uring invert their syscall counts by regime (46.6k vs 31.3k
   paced; 20.2k vs 40.3k saturated). No latency winner is claimed.
10. The mutex baseline is more *power-efficient* (8–9 % CPU vs ~101 %) and
    produced 10/10 clean runs versus 1/10 for spsc/default/epoll. The clean-run
    difference is almost certainly time-clustered host noise (each config was
    measured in its own window), not a queue property; it is listed so the raw
    table is not over-read.

### Inconclusive

11. **False-sharing throughput.** Best-case 18.11 (padded) vs 16.94 M/s
    (packed), 1.07×. The p99/p99.9 difference (0.28/5.0 µs vs 5.8/30.4 µs) is
    consistent with the mechanism but not a controlled result on this host.
12. Pinning improving end-to-end tails. The mechanism is proven; the outcome
    is not observable on this host.

### Not measured (blocked or out of scope)

13. Hardware counters (`perf`), real-time scheduling (`SCHED_FIFO`), governor,
    turbo and SMT control — all blocked by lack of privileges.
14. Multi-producer / MPSC, WAL / persistence, 60 s sustained stress — excluded
    by scope decisions, not by failure.

---

## 9. Would a VM or a quiet machine help?

**A VM: no. A freshly rebooted, otherwise-idle machine: yes.**

- A VM on the same laptop does not isolate anything: the vCPUs are scheduled
  by the same host kernel on the same cores, so you add a second layer of
  scheduling latency on top of the problem you are trying to remove. Steal
  time on the vCPU shows up as the same multi-millisecond gaps, now
  unattributable.
- A remote/cloud VM is worse for this project: no control over CPU model,
  frequency, SMT or neighbours, and `perf_event_paranoid` / PMU access is
  usually unavailable. Tail measurements become less trustworthy, not more.
- A VM *is* useful for one thing only: proving the build and tests are
  portable. It is not a measurement platform.

What a quiet physical machine changes, based on the isolation experiment:

| Would improve | Would not change |
|---|---|
| Clean-run count (expect ~10/10 for every config) | Governor = `powersave` (no root) |
| p99.9 tails (expect ~140–160 µs for spsc) | Turbo on, SMT on (no root) |
| Variance between repetitions | No `perf` counters |
| Confidence in the false-sharing and allocator numbers | No `SCHED_FIFO` |

A quiet run would not create new claims; it would tighten the confidence
intervals on claims 1–8 and possibly resolve claim 11. It is worth doing if
the machine can be left alone for ~20 minutes, and the same scripts reproduce
everything.

### Quiet-run checklist

1. Reboot; do not open a browser, IDE, or a terminal that repaints heavily.
2. Log in on a TTY (or an idle terminal), run `./scripts/env.sh` and check
   `loadavg` is < 0.5.
3. `ME_LAT_RUNS=10 ME_THR_RUNS=5 ./scripts/run_matrix.sh --force`
4. `./scripts/run_experiments.sh && ./scripts/run_saturation.sh`
5. `./scripts/run_components.sh && ./scripts/run_probe.sh`
6. `python3 scripts/analyze.py && python3 scripts/analyze_components.py`
7. Commit the regenerated `results/` artifacts.

Total ≈ 20–25 minutes. If the machine must stay usable, run only the matrix
and saturation (≈12 minutes) and accept the existing component numbers.

---

## 10. Artifact index

| Path | Contents |
|---|---|
| `results/summary.md` | all generated pipeline tables (this report's §4–5) |
| `results/summary.csv` | one row per config/kind, medians |
| `results/analysis.json` | machine-readable groups, flags, checksums |
| `results/components-summary.md` | component medians (§3) |
| `results/components/*.txt` | raw component runs, 9 reps each |
| `results/env.txt` | host, compiler, blocked controls, commit |
| `results/host-isolation.txt` | the desktop-confinement experiment (§6) |
| `results/rt-attempt.txt` | the `SCHED_FIFO` denial |
| `results/raw/<config>/runs.csv` | every individual run (ignored by git) |

Plots (`results/plots/`):

| File | Shows |
|---|---|
| `cdf-queue.svg` | latency CDF, spsc vs mutex |
| `cdf-allocator.svg` | latency CDF, slab vs heap vs V1 book |
| `cdf-scheduling.svg` | latency CDF, default vs pinned |
| `cdf-network.svg` | latency CDF, epoll vs io_uring |
| `cdf-matrix-spsc.svg` | four spsc scheduling/network combinations |
| `bar-throughput.svg` | throughput by configuration |
| `bar-p999.svg` | p99.9 by configuration |
| `bar-saturation-p999.svg` | p99.9 vs offered rate |
| `bar-saturation-throughput.svg` | achieved vs offered rate |

---

## 11. One-paragraph summary

On a busy desktop, the lock-free SPSC pipeline sustains ≥1.6× the mutex
pipeline's throughput at 60 % of its engine CPU per order, with four orders of
magnitude fewer voluntary context switches in the producer (the mutex pushes
block, the SPSC pushes do not); the mutex queue collapses above
~2.9 M/s while the SPSC queue absorbs 6.4 M/s. At 200k orders/s, however, both
queues have the same median latency, and the tail is dominated by host ingress
scheduling (77–99 %), which a controlled desktop-confinement experiment
confirms by cutting p99.9 from 2.4 ms to 142 µs. The data-structure choice
matters far more than the queue at this load: the V3 book is 3× faster and
turns a 13.8 ms worst case into 26 µs. The slab allocator is 1.4–1.5× faster
in isolation but invisible end-to-end. These are the claims the recorded
evidence supports; the throughput ceiling of the SPSC path is *above* what the
current client can drive, so the true limit remains unmeasured.
