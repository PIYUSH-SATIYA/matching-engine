# Final benchmark report — low-latency matching engine

Status: measurement complete on `agent/final-benchmark-suite`. This revision
reports the **idle-TTY session** (commit `603be66`, session
`20261114T181631`), which supersedes the earlier busy-desktop pass. All raw
data, generated tables and plots live under `results/` and can be regenerated
from `scripts/`. Nothing in the older design documents was edited.

The headline difference from the previous revision: the multi-millisecond
p99.9 tails were **not** a property of the engine. Running the same binaries
from a TTY with no graphical session and the platform profile set to
`performance` cut the integrated p99.9 from 1.3–2.8 ms to 86–128 µs and
produced 120 of 122 clean runs. Everything else — the queue ordering, the
book ordering, the allocator ordering — reproduced.

---

## 0. Executive summary

| Question | Answer from this session |
|---|---|
| Does the SPSC queue beat the mutex queue? | **Yes on throughput and tail, no on median.** 5.86 vs 3.34 M/s unthrottled; p99 38 vs 71 µs; median latency statistically indistinguishable. |
| Where does the latency go? | **Not in matching.** Median 25 µs total, 3.1 µs of which is parse→match. The rest is ingress and wake-up. |
| Does the book design matter? | **More than anything else measured.** V3 is 2.9× faster and turns an 11 ms worst case into 27 µs. |
| Does the allocator matter? | In isolation yes (1.36–1.57×); end-to-end at 200k/s, **not measurably**. |
| Does pinning matter? | It removes run-queue wait (2.2 ms → 0.22 ms) but the end-to-end tail difference is not statistically significant (p = 0.14). |
| Does the host matter? | **More than any code change.** Same binary, same config: p99.9 1.79 ms (busy desktop) → 85.6 µs (idle TTY). |
| Can the SPSC pipeline be saturated by this harness? | No. It absorbed the maximum the client can offer (6.40 M/s) with **zero** full-queue waits. |

---

## 1. What was measured

| Block | Shape | Runs |
|---|---|---|
| Integrated matrix | queue {mutex, spsc} × scheduling {default, pin} × network {epoll, io_uring} | 8 configs × 10 latency + 5 throughput |
| Allocator in the pipeline | fast-slab / fast-heap / V1 book | 3 × 10 latency + 5 throughput |
| Saturation sweep | queue {spsc, mutex} × offered rate {50k … 6.4M/s, unthrottled} | 2 × 9 × 3 |
| Components | matching core, queue+core, allocator isolation, false sharing | 4 × 9 reps |
| Probes | ingress attribution, client pinning, net pinning | 4 × 3 |

231 recorded runs. Each latency run sends 1,010,000 orders, each unthrottled
run 2,000,000, each 6.4 M/s saturation run 3,010,000.

### Workload and protocol

- One TCP client, deterministic seed 42, 64-byte batches when paced.
- 10,000 orders of warm-up discarded before any sample is recorded.
- Latency runs: open loop at 200,000 orders/s, 10 repetitions.
- Throughput runs: unthrottled, 5 repetitions.
- Saturation runs: fixed offered rate, 2 s measured window (shortened at high
  rates to stay inside the 4 M-sample recorder), 3 repetitions.
- Reported values are **medians across repetitions**, not means. Min–max
  ranges are printed wherever the spread matters.
- Latency is client-send → engine-complete, measured with `CLOCK_MONOTONIC`
  on the same host. Both timestamps are taken inside the process; no clock
  synchronisation is involved.

### Instrumentation definitions

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

A "clean run" is the measurement-quality gate. Delayed runs are recorded
rather than deleted, so the matrix stays auditable; configurations are ranked
on clean runs only.

Statistical statements use a two-sided permutation test on the difference of
medians (20,000 resamples, clean runs only). With n = 9–10 per configuration
this resolves differences of roughly 30 % or more; smaller differences are
reported as "not distinguishable".

---

## 2. Conditions, and why they are part of the result

| Item | Value |
|---|---|
| Kernel | `7.2.9-arch1-1`, `PREEMPT_DYNAMIC` |
| CPU | 4 P-cores × 2 SMT (≤ 4.4 GHz) + 4 E-cores (≤ 3.1 GHz), 12 threads |
| Compiler | GCC 16.2.1, Release (`-O3 -DNDEBUG`), no sanitizers |
| liburing | 2.15 |
| Session | TTY login, no graphical session, no browser, no editor |
| `loadavg` at start | 0.21 / 0.05 / 0.02 |
| Platform profile | `performance` (EPP `performance` on all 12 CPUs), on AC |
| Governor | `powersave` (intel_pstate), turbo enabled, SMT enabled |
| `SCHED_FIFO` | **denied** (`EPERM`, `rtprio_limit=0`) |
| `perf` | **unavailable** (binary missing, `perf_event_paranoid=2`) |
| `strace` | unavailable |

### Thermal and frequency evidence

A 5-second sampler recorded package temperature and the fastest P-core
frequency for the whole 16-minute session (`results/thermal-<ts>.csv`, 199
samples):

| Metric | Value |
|---|---|
| Package temperature | min 47 °C, median 55 °C, max **71 °C** |
| Fastest P-core frequency | median 4.37 GHz, max 4.42 GHz |
| Samples at ≥ 95 °C | none |
| Samples below 4.0 GHz | 5 of 199, all between benchmark runs or during the single-threaded book bench |

**There was no thermal throttling**, so the matrix ordering (spsc first,
mutex later) does not bias the queue comparison through a drifting clock.

### What is still blocked

Frequency still moves between runs (2.2–4.4 GHz) because `powersave` and
`intel_pstate` are in charge; `isolcpus`, `nohz_full`, SMT control and
real-time priorities all need root. The measurements are therefore a *fair
A/B under identical conditions*, not an absolute latency floor.

---

## 3. Reproducibility across host conditions

Same binaries, same configurations, three host conditions. This is the most
important table in the report because it tells you which conclusions are
properties of the code and which were properties of the machine.

| Condition | spsc/pin/epoll p99.9 | Clean runs | Unthrottled spsc | Unthrottled mutex |
|---|---|---|---|---|
| Busy desktop (~380 % CPU terminal) | 1.79 ms | 1/10 | 4.72 M/s | 2.99 M/s |
| Desktop confined to E-cores (n=3, `host-isolation.txt`) | 142 µs | 3/3 | – | – |
| **Idle TTY, performance profile** | **85.6 µs** | **9/10** | **5.86 M/s** | **3.34 M/s** |

Two things follow:

1. **The tail was the host.** Removing desktop contention improved p99.9 by
   21× and clean runs from 1/10 to 9–10/10 across the board, without touching
   the code. Any claim about millisecond-scale tails from the previous session
   was a claim about the desktop.
2. **The ordering was not the host.** Every ordering that mattered survived
   the change: spsc > mutex on throughput (1.58× before, 1.75× now), V3 > V1 on tail,
   slab > heap in isolation. The absolute numbers moved up 6–24 % because the
   pinned configurations were no longer losing their cores to the desktop.

The median latency, notably, did **not** improve: it was 22.6–33.0 µs across
the eight configurations before and 23.8–29.1 µs now. An idle machine parks
cores, so the per-wake-up cost does not fall; only the tail does.

---

## 4. Component results

### 4.1 Matching core (single thread, no queue, no network)

`results/components/bench-order-book.txt`, 2 M operations, 9 reps, pinned.

| book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| V1 (`map` + `list` + `unordered_map`) | 11.63 M/s | 81 ns | 420 ns | 595 ns | **10.97 ms** |
| V3 (array + slab + bitmap) | **33.68 M/s** | 94 ns | 247 ns | 374 ns | **26.7 µs** |

Read this correctly: V3 is **not** faster at the median (94 vs 81 ns). It
removes the tail. V1's 11 ms spike is a tree rebalance / allocator event under
the same deterministic workload; V3's worst case is 26.7 µs. Both produce
identical trade counts and notionals (1,093,640 trades; 88,304 cancels hit,
512,136 miss).

### 4.2 Allocator in isolation

`results/components/bench-allocator.txt`, 2 M operations on 64-byte nodes,
live set 8192, 9 reps.

| allocator | alloc ns/op | alloc p99.9 | alloc sd | free ns/op | free p99.9 | free sd |
|---|---|---|---|---|---|---|
| aligned `::operator new/delete` | 17.14 | 44 ns | 27.5 | 22.19 | 136 ns | 86.2 |
| `SlabAllocator` | **12.59** | **16 ns** | **6.9** | **14.13** | **24 ns** | **8.5** |

1.36× faster to allocate, 1.57× to release, with a quarter of the variance.
Absolute values include ~20–25 ns of clock cost in both variants, so the ratio
and the spread are the meaningful numbers.

### 4.3 Queue + core, no network

`results/components/bench-pipeline.txt`, 1.01 M orders paced at 200 k/s, 9 reps.
This is the floor the integrated pipeline is measured against.

| queue + book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| spsc + V3 | **7.35 M/s** | **299 ns** | 1.6 µs | **13.2 µs** | 160 µs |
| spsc + V1 | 5.77 M/s | 247 ns | 836 ns | 5.40 ms | 10.24 ms |
| mutex + V3 | 3.49 M/s | 1.6 µs | 3.9 µs | 14.6 µs | 404 µs |
| mutex + V1 | 3.27 M/s | 1.6 µs | 3.6 µs | 6.26 ms | 11.02 ms |

The queue accounts for a 2.1× throughput difference and a 5.4× median-latency
difference once the network is removed. The book accounts for the tail: with
V1 in the pipeline, p99.9 is 5.4–6.3 ms regardless of queue.

### 4.4 False sharing on the SPSC boundary

`results/components/bench-false-sharing.txt`, 5 M orders per rep, 9 reps.
Two buffers with byte-identical push/pop logic differ only in layout: `padded`
gives each ownership domain its own cache line (what
`include/spsc_ring_buffer.hpp` does), `packed` puts all four indices adjacent.

| layout | throughput median | throughput best | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| padded | 8.59 M/s | 9.66 M/s | **147–163 ns** | 244 ns | 17.8 µs |
| packed | **10.28 M/s** | **11.78 M/s** | 177–853 ns | 530 ns | 26.2 µs |

**The honest reading is a trade-off, not a win.** Per-item latency favours
padding in 9 of 9 reps here (median 153 vs 197 ns) and in 9 of 9 reps of the
previous session — 18/18 overall, two-sided sign test p ≈ 8 × 10⁻⁶.
Throughput moves the other way in 8 of 9 reps here (0.75–1.09×,
padded/packed), while the previous, busier session had padding ahead in 3 of
9 (0.41–2.12×) and this one in 1 of 9. The throughput direction is not stable
across host conditions, so no throughput claim is made.

The plausible mechanism — not verified here — is that packing lets the slot
write and the index publish land on the same line, which reduces the number
of line transfers per item (higher rate) but puts both threads on one line
(higher per-item latency). The production queue uses the padded layout
*and* caches the peer index locally, so the remote line is touched only when
the queue looks empty or full; this benchmark does not measure that path, so
it cannot be used to justify the production layout. Padding is kept because
the mechanism is sound and the alternative has no demonstrated advantage, not
because this experiment proves it.

---

## 5. Integrated pipeline

### 5.1 Throughput (unthrottled, 5 runs, medians)

| config | orders/s | engine CPU/order | net CPU/order | net `nvcsw` | kernel ops/run |
|---|---|---|---|---|---|
| spsc / pin / epoll | **5.86 M/s** | **0.17 µs** | 0.17 µs | 12 | 20,190 |
| spsc / pin / uring | 5.48 M/s | 0.18 µs | 0.18 µs | 12 | 40,328 |
| spsc / default / epoll | 5.29 M/s | 0.19 µs | 0.19 µs | 15 | 20,194 |
| spsc / default / uring | 5.14 M/s | 0.19 µs | 0.19 µs | 15 | 40,328 |
| mutex / pin / uring | 3.38 M/s | 0.30 µs | 0.21 µs | **157,151** | 40,328 |
| mutex / pin / epoll | 3.34 M/s | 0.30 µs | 0.20 µs | 172,731 | 20,170 |
| mutex / default / epoll | 3.19 M/s | 0.32 µs | 0.20 µs | 159,756 | 20,168 |
| mutex / default / uring | 3.12 M/s | 0.32 µs | 0.20 µs | 198,511 | 40,328 |

**Ceiling caveat.** These "unthrottled" figures are *client-limited*. The
unthrottled client sends 1024-order batches, blocks on a full socket, and
delivered 5.5–5.9 M orders/s; the paced 6.4 M/s client sends 64-order
batches, never blocks, and delivered **6.40 M/s, which the spsc pipeline
consumed with zero full-queue waits** (§6). The server ceiling is therefore
above 6.4 M/s, not 5.86 M/s. The mutex numbers are a genuine service result;
the spsc numbers are a harness limit.

The mechanism behind the gap is measured, not inferred: the network thread
made **157k–199k voluntary context switches** pushing into the mutex queue
(it blocks inside `push`) versus **12–15** for spsc, and the engine burned
0.30–0.32 vs 0.17–0.19 µs of CPU per order. Engine involuntary switches are
small in both cases; the cost is the producer's block/wake cycle.

### 5.2 Latency at 200 k orders/s (medians of 10 runs)

| config | p50 | p95 | p99 | p99.9 | clean runs |
|---|---|---|---|---|---|
| spsc / pin / epoll | 25.0 µs | 34.5 µs | **37.6 µs** | **85.6 µs** | 9/10 |
| spsc / pin / uring | 24.8 µs | 33.6 µs | 36.7 µs | 96.4 µs | 10/10 |
| spsc / default / epoll | 26.8 µs | 36.2 µs | 38.9 µs | 127.7 µs | 10/10 |
| spsc / default / uring | **24.2 µs** | 31.1 µs | 36.0 µs | 121.7 µs | 10/10 |
| mutex / default / epoll | 24.3 µs | 64.7 µs | 71.7 µs | 158.5 µs | 10/10 |
| mutex / default / uring | 23.8 µs | 60.3 µs | 69.7 µs | 150.6 µs | 10/10 |
| mutex / pin / epoll | 27.2 µs | 64.9 µs | 70.9 µs | 105.3 µs | 10/10 |
| mutex / pin / uring | 29.1 µs | 66.0 µs | 71.1 µs | 113.7 µs | 10/10 |
| allocator: fast-slab | 26.6 µs | 34.9 µs | 37.5 µs | 98.7 µs | 10/10 |
| allocator: fast-heap | 24.5 µs | 32.9 µs | 35.9 µs | 119.5 µs | 10/10 |
| allocator: V1 book | 24.4 µs | 32.3 µs | 36.6 µs | **6.52 ms** | 10/10 |

The queue comparison is now statistically resolvable *at p99 and not at
p50 or p99.9*:

| comparison (clean runs) | spsc | mutex | p |
|---|---|---|---|
| p50, default / epoll | 26.8 µs | 24.3 µs | 0.14 — not distinguishable |
| p99, default / epoll | 38.9 µs | 71.7 µs | **0.0009** |
| p99, pin / epoll | 38.3 µs | 70.9 µs | **0.0017** |
| service p99.9, pin / epoll | 20.7 µs | 42.0 µs | **0.0007** |
| total p99.9, pin / epoll | 86.8 µs | 105.3 µs | 0.14 — not distinguishable |

That pattern is the whole story of the two queues at moderate load: the
mutex is as fast as spsc in the typical case, but every hand-off that has to
sleep adds a scheduler round trip, which is visible at p95/p99 and in the
*matching-side* tail, and invisible in the median.

### 5.3 Latency decomposition (medians, same runs)

Total p99.9 ≈ arrival p99.9 + service p99.9 (percentile sums are approximate).

| config | total p99.9 | arrival p99.9 | service p99.9 | arrival share |
|---|---|---|---|---|
| spsc / pin / epoll | 85.6 µs | 43.9 µs | 23.6 µs | 51 % |
| spsc / pin / uring | 96.4 µs | 71.2 µs | **16.1 µs** | 74 % |
| spsc / default / epoll | 127.7 µs | 83.8 µs | 20.7 µs | 66 % |
| spsc / default / uring | 121.7 µs | 86.4 µs | 27.1 µs | 71 % |
| mutex / pin / epoll | 105.3 µs | 58.7 µs | 42.0 µs | 56 % |
| mutex / pin / uring | 113.7 µs | 60.9 µs | 42.3 µs | 54 % |
| mutex / default / epoll | 158.5 µs | 60.2 µs | **102.6 µs** | 38 % |
| mutex / default / uring | 150.6 µs | 69.2 µs | 95.0 µs | 46 % |
| allocator: V1 book | 6.52 ms | 47.3 µs | **6.50 ms** | 1 % |

Median over the four configurations, **service** p99.9: spsc 20.7 µs, mutex
68.6 µs (3.3×). At the median, spsc service is 3.0–3.1 µs; the integrated
median of ~25 µs is therefore **86–88 % ingress and wake-up**, not matching.

### 5.4 Allocator (fixed: spsc, epoll, pinned)

| config | p50 | p99 | p99.9 | clean runs | throughput |
|---|---|---|---|---|---|
| fast-slab | 26.6 µs | 37.5 µs | 98.7 µs | 10/10 | 5.50 M/s |
| fast-heap | 24.5 µs | 35.9 µs | 119.5 µs | 10/10 | 5.08 M/s |
| V1 book | 24.4 µs | 36.6 µs | 6.52 ms | 10/10 | 4.32 M/s |

The isolated allocator is 1.36–1.57× faster, but **end-to-end the difference
is not resolvable**: 98.7 vs 119.5 µs p99.9, permutation p = 0.38 with ranges
of 57.7–147.5 and 67.9–150.5 µs. At this rate allocation is off the critical
path. The V1 row is the control that shows what a pathological allocator
*does* do: 6.52 ms p99.9, 6.50 ms of it in the engine.

### 5.5 Scheduling and affinity

| fixed spsc / fast-slab / epoll | clean | p50 | p99 | p99.9 | engine run-queue wait | engine `nivcsw` |
|---|---|---|---|---|---|---|
| default scheduler | 10/10 | 26.8 µs | 38.9 µs | 127.7 µs | 2.20 ms | 27 |
| engine pinned to CPU 2 | 9/10 | 25.0 µs | 37.6 µs | 85.6 µs | **0.22 ms** | 26 |

Pinning does what it is designed to do — engine run-queue wait drops 10×, and
the engine's involuntary switches are already low. The end-to-end p99.9
difference (128 → 86 µs) is a 33 % improvement that does **not** reach
significance at n = 10 (p = 0.14). The network thread still floats, and it is
the component that absorbs host interference.

One measurement artifact is worth recording: for the *blocking* queue, the
engine's run-queue counter is large and erratic in the pinned configuration
(median 72.6 ms, range 0.08–154 ms) versus 0.06 ms unpinned. This is the
accumulated wake-up delay of ~17k voluntary sleeps per run, not a stall —
median latency stays at 27 µs. `schedstat` counts the time a woken thread
waits to be placed on a CPU, which the spinning consumer never accrues and
the sleeping one always does.

### 5.6 epoll vs io_uring

Kernel operations per run, medians:

| regime | epoll | io_uring |
|---|---|---|
| paced 200 k/s | 47,346–47,351 | 31,569–31,570 |
| unthrottled | 20,168–20,194 | 40,328 |

The inversion is a property of the code paths, not noise: at 200 k/s epoll
calls `epoll_wait` + `recv` per wake-up while io_uring submits one multi-shot
receive and collects many completions; at saturation epoll's drain loop
amortises one wait over many `recv`s, while io_uring still pays one submit per
receive. Latency differences between the two are not consistent across the
matrix (p99 differs by ±20 % in both directions depending on the pair), so no
latency winner is claimed.

### 5.7 CPU accounting

| queue | engine CPU, 200 k/s | engine CPU, unthrottled | CPU/order |
|---|---|---|---|
| spsc | ~101 % (spins) | ~100 % | 0.17–0.19 µs |
| mutex | 8–9 % (sleeps on condvar) | 100 % | 0.30–0.32 µs |

This is the clearest OS-level trade-off in the project. The lock-free queue
buys tail latency and throughput by keeping a core busy; the mutex baseline
is power-friendly and equally fast at the median, but pays a scheduler round
trip on every hand-off and collapses under sustained overload (§6).

---

## 6. Saturation

Offered rate is what the client paced; achieved is what the server completed.
Each cell is the median of 3 runs.

| offered | spsc achieved | spsc p50 | spsc full waits | mutex achieved | mutex p50 | mutex full waits |
|---|---|---|---|---|---|---|
| 50 k/s | 50 k/s | 112.1 µs | 0 | 50 k/s | 308.0 µs | 0 |
| 100 k/s | 100 k/s | 23.1 µs | 0 | 100 k/s | 64.6 µs | 0 |
| 200 k/s | 200 k/s | 22.8 µs | 0 | 200 k/s | 25.6 µs | 0 |
| 400 k/s | 400 k/s | 11.7 µs | 0 | 400 k/s | 15.0 µs | 0 |
| 800 k/s | 800 k/s | 12.3 µs | 0 | 800 k/s | 14.3 µs | 0 |
| 1.6 M/s | 1.6 M/s | 10.5 µs | 0 | 1.6 M/s | 13.7 µs | 0 |
| 3.2 M/s | 3.2 M/s | 10.2 µs | 0 | 3.2 M/s | 13.5 µs | 0 |
| 6.4 M/s | **6.40 M/s** | 10.3 µs | **0** | **3.32 M/s** (2 of 3 runs) | **32.7 ms** | **414k–495k** |
| unthrottled | 5.56 M/s | 12.9 ms | 1,841,316 | 3.33 M/s | 21.5 ms | 250k |

Four findings:

1. **The SPSC queue is not close to saturation.** It returned 6.40 M/s in
   3 of 3 runs with **zero** full-queue waits; the harness cannot offer more.
   The cost is visible on the other side: 43 k of 3.0 M pops (1.4 %) found the
   ring empty and had to spin.
2. **The mutex path is bistable at 6.4 M/s, not simply capped.** Two of three
   runs collapsed to ~3.3 M/s with a 32.7 ms median, 414k–495k blocking
   pushes and 208k–259k voluntary switches in the network thread. The third
   run kept up: 6.01 M/s, 0.62 ms median, 26.7 k full waits — but still with a
   **30.9 ms worst case** at the end of the run. With n = 3 this is a warning,
   not an estimate: the mutex ceiling is somewhere between 3.2 M/s (stable,
   zero waits) and 6.4 M/s (collapse in 2 of 3), and *whether* it collapses
   appears to depend on which sleep/wake pattern the two threads fall into.
3. **The closed-loop throughput number understates the comparison.** The
   unthrottled client blocks on the socket, so the mutex pipeline settles at
   3.3 M/s in 3 of 3 runs; the open-loop sweep shows the same path can reach
   6.0 M/s when it happens not to collapse. Reported alone, either number
   would be misleading.
4. **Latency is not monotonic in offered rate.** Both queues get *faster* as
   the rate rises from 50 k/s to ~3.2 M/s (mutex 308 → 13.5 µs, spsc 112 →
   10.2 µs) because wake-up cost is amortised over bigger batches. Below
   ~200 k/s the blocking queue is 2.7× slower than the spinning one at the
   median — the idle case is where the mutex loses most, not the busy case.

Caveat for the unthrottled rows: end-to-end latency there includes kernel
socket buffers and client send blocking, so those rows measure *pipeline
response time* under a closed loop, not queue occupancy.

---

## 7. Where the remaining latency comes from

Three independent lines of evidence agree:

1. **In-process attribution.** `net_ingest_slow` is zero in 120 of 122 runs.
   The two exceptions are single startup episodes: 64 orders in the first run
   of the first configuration (`q-spsc_sched-pin_net-epoll` run 1, max 2.17 ms,
   network run-queue wait 0.0 ms — the client was descheduled before `send`,
   not the server after receipt) and 192 orders in `probe-net-pinned` run 3
   (max 1.67 ms).
2. **Arrival/service split.** In the spsc configurations, arrival accounts for
   51–74 % of p99.9 and service is 16–27 µs; the component pipeline with no
   network at all has p99.9 of 13.2 µs, which is the same order.
3. **The confinement experiment** (`results/host-isolation.txt`): confining a
   ~380 % desktop process to the E-cores moved p99.9 from 2.4 ms to 142 µs
   while the engine's run-queue wait stayed at 0.29 ms. The network thread was
   the delayed component, and the improvement was not specific to the queue.

Now that the host is quiet, what remains is **~25 µs median and 86–128 µs
p99.9** for a pipeline whose matching step takes 3 µs. That residual is the
wire: one epoll wake per batch, a `recv`, a parse, a cross-core hand-off and a
wake-up. Removing it would require busy-polling the NIC or kernel-bypass, both
outside this project's scope.

---

## 8. Correctness and parity

- 7/7 unit/integration tests pass in Release and under ASan/UBSan; 2/2
  ring-buffer tests pass under TSan; epoll and io_uring produce identical
  results in `verify.sh`.
- Identically seeded matrix runs produce **one** checksum group for latency
  (788,580 trades, qty 20,127,113, notional 201,270,962,137) and **one** for
  throughput (1,561,056 trades, qty 39,843,705, notional 398,436,546,776)
  across all queue, allocator, scheduling and network variants.
- Those checksums are identical to the previous session's, which confirms the
  workload itself did not change between sessions.
- Component benches reproduce the same result across all four queue/book
  combinations.

Correctness is therefore not conditional on any performance number.

---

## 9. What can be claimed

### Demonstrated (direct measurement, reproducible)

1. The SPSC queue sustains **5.86 vs 3.34 M/s** (1.75×) at **0.17 vs 0.30 µs**
   engine CPU per order, with **12 vs 157k–199k** voluntary context switches in
   the producer per run.
2. At 6.4 M/s offered, SPSC held **6.40 M/s with zero full-queue waits in 3 of
   3 runs**; the mutex path collapsed in 2 of 3 (32.7 ms median, 414k–495k
   blocking pushes) and reached 6.01 M/s in the third with a 30.9 ms worst
   case.
3. At 200 k orders/s the SPSC queue has **half the p99** (38 vs 71 µs,
   p = 0.001) and **one third the service-tail p99.9** (21 vs 69 µs,
   p = 0.0007), while its median latency is **not distinguishable**
   (26.8 vs 24.3 µs, p = 0.14).
4. Below ~200 k/s the blocking queue is **2.7× slower at the median**
   (308 vs 112 µs at 50 k/s) because each order pays a full sleep/wake.
5. The V3 book is **2.9× faster** than V1 (33.68 vs 11.63 M/s) and turns an
   **10.97 ms → 26.7 µs** worst case; in the integrated pipeline V1 alone
   produces 6.52 ms p99.9, 99.7 % of it inside the engine.
6. The slab allocator is **1.36× faster to allocate, 1.57× to release** with
   roughly a quarter of the variance, and is **not resolvable end-to-end** at
   200 k/s (98.7 vs 119.5 µs, p = 0.38).
7. Engine pinning cuts engine run-queue wait **2.20 → 0.22 ms**; the end-to-end
   p99.9 improvement (128 → 86 µs) is not statistically significant at n = 10.
8. epoll and io_uring invert their syscall counts by regime (47.3k vs 31.6k
   paced; 20.2k vs 40.3k saturated). No latency winner is claimed.
9. The pipeline floor is **299 ns median / 13.2 µs p99.9** without the network;
   the integrated median of ~25 µs is therefore 86–88 % ingress and wake-up.
10. 120 of 122 latency runs were clean; the two exceptions are isolated startup
    episodes, each recorded with the order count and delay.
11. All configurations are numerically identical and pass tests/sanitizers.

### Supported, but not proven on this host

12. **Padding versus packing on the SPSC boundary.** Per-item latency favours
    padding in 18 of 18 reps across two sessions (two-sided sign test
    p ≈ 8 × 10⁻⁶); throughput direction reversed between sessions, so the
    production layout is justified by mechanism, not by a measured throughput
    win.
13. Repeatability of the mutex ceiling. n = 3 with a bimodal outcome is not
    enough to estimate collapse probability.

### Inconclusive / not measured

14. Which of epoll and io_uring is better for tail latency.
15. Whether pinning the network thread as well would help (it was deliberately
    left floating; the probes show floating is not the problem once the host is
    quiet).
16. Hardware counters (`perf`), real-time scheduling (`SCHED_FIFO`), governor,
    turbo, SMT and `isolcpus` — all blocked by lack of privileges.
17. Multi-producer / MPSC, WAL / persistence, and a 60 s sustained stress run —
    excluded by scope decision, not by failure.

---

## 10. Threats to validity

- **One host, one kernel, one compiler.** Every number is conditional on
  `7.2.9-arch1-1`, GCC 16.2.1 and this laptop's P/E-core topology. Ratios are
  more portable than absolutes.
- **Frequency still varies.** `powersave` leaves the governor in charge; clocks
  held 4.2–4.4 GHz during runs but idle at 0.8 GHz between them. Run-to-run
  variance of ±5 % in throughput is consistent with this.
- **SMT is on.** The engine shares a physical core with its sibling; a
  neighbour on that sibling would roughly halve its throughput. This was not
  controlled and not observed.
- **Client-limited throughput.** The unthrottled client cannot drive the SPSC
  path past ~5.9 M/s; the 6.4 M/s paced sweep is the only evidence about the
  ceiling, and it is a lower bound.
- **Small n for saturation.** 3 repetitions cannot characterise a bistable
  system; the mutex collapse percentage is unknown.
- **Percentile sums are approximate.** Total p99.9 is reported separately from
  arrival + service p99.9, not derived from it.

---

## 11. Artifacts and reproduction

| Path | Contents |
|---|---|
| `results/summary.md` | all generated pipeline tables (§5–6) |
| `results/summary.csv` | one row per config/kind, medians |
| `results/analysis.json` | machine-readable groups, flags, checksums |
| `results/components-summary.md` | component medians (§4) |
| `results/components/*.txt` | raw component runs, 9 reps each |
| `results/env.txt` | host, power profile, EPP/frequency, thermal snapshot, commit |
| `results/host-isolation.txt` | the desktop-confinement experiment (§3, §7) |
| `results/rt-attempt.txt` | the `SCHED_FIFO` denial |
| `results/bench-session-<ts>.log` | full transcript of the session (gitignored) |
| `results/thermal-<ts>.csv` | 5 s temperature/frequency samples (gitignored) |
| `results/raw/<config>/runs.csv` | every individual run (gitignored) |
| `results/raw-prev-<ts>/` | the previous session, kept for comparison (gitignored) |

Plots (`results/plots/`, gitignored, regenerated by `scripts/analyze.py`):
`cdf-queue`, `cdf-allocator`, `cdf-scheduling`, `cdf-network`,
`cdf-matrix-spsc`, `bar-throughput`, `bar-p999`, `bar-saturation-p999`,
`bar-saturation-throughput`.

### Reproducing this exact session

One command does everything — preservation of the previous data, environment
capture, Release rebuild, all five measurement phases, both analyses and a
thermal sampler:

```sh
./scripts/run_session.sh
```

It refuses to start on a dirty tree, sets the platform profile to
`performance`, prints each phase's exit code, and writes a transcript to
`results/bench-session-<ts>.log`. Run it from a TTY, or from an empty
workspace with no compositor repainting, and leave the machine alone for the
~16 minutes it takes. The previous session's data is moved to
`results/raw-prev-<ts>/` rather than deleted, so comparisons stay possible.

The individual phases remain available if only one is wanted:
`run_matrix.sh`, `run_experiments.sh`, `run_saturation.sh`,
`run_components.sh`, `run_probe.sh`, then `analyze.py` and
`analyze_components.py`.

---

## 12. One paragraph

On an idle machine with the power profile fixed at `performance`, the lock-free
SPSC pipeline sustains 1.75× the mutex pipeline's throughput at 56 % of its
engine CPU per order and with four orders of magnitude fewer producer context
switches, absorbs the maximum rate the client can offer (6.40 M/s, zero full
waits), and has half the p99 and one third the matching-side tail — while being
statistically indistinguishable at the median, because the median is dominated
by ingress, not by the queue. The mutex path is not merely slower at overload;
at 6.4 M/s it is bistable, either keeping up at 6.0 M/s or collapsing to 3.3 M/s
with 33 ms medians, and its worst case is the idle case (2.7× slower than spsc
at 50 k/s) because every hand-off sleeps. The data-structure choice inside the
book matters more than the queue at moderate load: V3 is 2.9× faster and turns
an 11 ms worst case into 27 µs. The slab allocator is 1.4–1.6× faster in
isolation and invisible end-to-end. And the single largest factor in the
previous revision's numbers was the host: the same binaries moved from 1.79 ms
to 85.6 µs p99.9 when the desktop was removed, which is why the engine now has
a measured latency budget it can reason about instead of a tail it could only
attribute.
