# Lock-Free, SPSC Order Book Matching Engine

## Final Project Report

| Field | Details |
|---|---|
| **Course** | Operating Systems |
| **Submission Type** | Final Report (replaces the earlier proposal) |
| **Team Size** | 5 members |
| **Language / Platform** | C++20 / Linux |
| **Repository** | `matching-engine` |
| **Measured on** | Intel i5-12450HX, 12 hardware threads, kernel 7.2.9-arch1-1, GCC 16.2.1 |
| **Raw results** | `results/summary.md`, `results/components-summary.md`, `results/env.txt` |

## Team Members

| Member | Name | Roll No. |
|---|---|---|
| A | Piyush Satiya | 202451130 |
| B | Prayag Kirar | 202451130 |
| C | Sanskar Sharma | 202451148 |
| D | Sorabh Baldeva | 202451156 |
| E | Ved Gupta | 202451166 |

---

## 1. Executive Summary

We built a single-threaded price–time-priority matching engine fed by a network
thread through either a mutex ring buffer or a lock-free SPSC ring buffer, behind
either an `epoll` or an `io_uring` ingestion path, with the order book implemented
twice (a reference `std::map`/`std::list` book and an array/bitmap/slab book).
Every configuration is exercised by the same harness on the same host, and every
order is timestamped twice **inside the server** so that latency can be split into
the part spent before the engine sees the order and the part spent inside it.

Four results carry the report:

1. **The engine is not the bottleneck, and the tail is not in the matcher.**
   Measured in isolation, the optimized book matches or rests an order in
   **94 ns (p50)** and **374 ns (p99.9)**; producer→queue→engine with no network
   is **299 ns (p50)** and **13.2 µs (p99.9)**. The full TCP pipeline is
   **25.0 µs (p50)** and **86.8 µs (p99.9)**, of which the arrival stage
   (client send → network thread parse) accounts for **51–74 %** of the p99.9.
   At the median the engine's share is ~3 µs, so **86–88 % of typical latency is
   ingress and wake-up**, not matching.

2. **The queue choice is invisible at p50 and dominant in the tail and in
   throughput.** SPSC vs mutex, everything else fixed: p50 26.8 µs vs 24.3 µs
   (indistinguishable, permutation p = 0.14); p99 **38.9 µs vs 71.7 µs**
   (p = 0.0009); *service* p99.9 across the four configurations **20.7 µs vs
   68.6 µs** (p = 0.0007 for the pinned pair); throughput **5.86 M/s vs
   3.34 M/s**; engine CPU **0.17–0.19 µs vs 0.30–0.32 µs** per order; voluntary
   context switches in the network thread **12–15 vs 157 000–199 000**. A
   condvar queue must be woken and scheduled for every batch; an SPSC consumer
   that is already spinning pays nothing.

3. **The data structure inside the book matters more than the queue at moderate
   load.** V3 is **2.9× the throughput** of V1 (33.68 vs 11.63 M/s) and turns a
   **10.97 ms** worst case into **26.7 µs**; in the integrated pipeline the V1
   book alone produces **6.52 ms p99.9**, 99.7 % of it inside the engine. The
   slab allocator, by contrast, is 1.36–1.57× faster in isolation and **not
   resolvable end-to-end** (98.7 vs 119.5 µs, p = 0.38).

4. **At overload the two queues separate qualitatively, not just
   quantitatively.** At 6.4 M orders/s offered, SPSC delivered **6.40 M/s with
   zero full-queue waits in 3 of 3 runs**. The mutex path is **bistable**: two of
   three runs collapsed to ~3.3 M/s with 32.7 ms medians and 414k–495k blocking
   pushes, while one held 6.01 M/s with a 0.62 ms median but still a 30.9 ms
   worst case. Its worst regime, however, is the *idle* one: at 50 k orders/s it
   is 2.7× slower than SPSC at the median (308 vs 112 µs) because every hand-off
   sleeps.

Affinity without isolation did not help: pinning the engine cut its run-queue
wait from 2.20 ms to 0.22 ms, but the end-to-end p99.9 improvement
(127.7 → 86.8 µs) is not statistically significant at n = 10 (p = 0.14).
`SCHED_FIFO` was denied by host policy, so the realtime arm is a documented null
result. A controlled confinement experiment (Section 6.12) shows that the
multi-millisecond outliers seen under desktop contention are host ingress delay,
not a property of the pipeline: confining the competing desktop process to the
E-cores moved the same configuration from **2.40 ms to 142 µs** p99.9 while the
engine's own CPU time was unchanged.

---

## 2. Project Overview and Central Question

An order book stores outstanding orders by price. A buy order means "buy up to
*quantity* at *price* or better"; a sell order is the mirror. When an incoming
order crosses a resting order on the other side, a trade executes at the resting
order's price. Price–time priority means better price first, and FIFO within a
price level.

This is an operating-systems project, not a finance project. The matching logic
is deliberately single-threaded and deterministic; everything interesting happens
at the boundaries where the OS is involved:

> **What happens to latency and tail latency when we change synchronization,
> memory allocation, CPU scheduling and I/O one at a time, and can we explain the
> result from OS mechanisms rather than from folklore?**

The same shape — many producers, a queue, one state owner — appears in telemetry
pipelines, packet processors and messaging systems, so the results transfer.

---

## 3. System as Built

```
 client (load_client, pinned to CPU 6)
        │  40-byte little-endian messages over TCP, TCP_NODELAY,
        │  each carrying a CLOCK_MONOTONIC send timestamp
        ▼
 network thread (floating by default)
        ├─ epoll: level/edge-triggered wait + drain loop of recv()
        └─ io_uring: single-shot recv SQE per completion
        │  stamps Order.enqueue_time, pushes
        ▼
 queue: mutex ring buffer (condvars)  OR  lock-free SPSC ring buffer
        │
        ▼
 engine thread (optionally pinned to CPU 2)
        ├─ OrderBookFast (V3): direct-indexed price array, intrusive FIFO levels,
        │   open-addressing id hash, 2-level bitmap for best price,
        │   64-byte nodes from a pre-allocated slab (or from operator new)
        └─ OrderBook (V1): std::map + std::list + std::unordered_map reference
        │
        ▼
 CountingSink → trades, traded quantity, notional (used as a checksum)
```

Two things make the system measurable rather than merely fast:

- **`Order.enqueue_time`.** The network thread stamps each order after parsing and
  before pushing. `submit_time` (from the client) → `enqueue_time` is the
  *arrival* component; `enqueue_time` → completion is the *service* component
  (queue wait + matching). Adding the field kept `Node` at exactly 64 bytes, so
  the measurement did not change the data structure it measures.
- **`/proc/thread-self/schedstat`.** At the end of each run the harness reads the
  second field, the cumulative time the thread was runnable but waiting for a
  CPU, for both the engine and the network thread (`engine_runq_ns`,
  `net_runq_ns`). That converts "the scheduler probably interfered" into a number.

---

## 4. Component Design

### 4.1 Matching engine and the two books

`MatchingEngine<Book, Sink>` is a template over the book and the event sink, so
the matching logic is identical for both implementations and cannot drift.

**V1 (reference).** `std::map<Price, std::list<Order>>` per side plus an
`std::unordered_map<OrderId, iterator>` for cancel. Correct, simple, and the
baseline that makes the optimized version's gains attributable.

**V3 (optimized).**

- Prices index a flat array `levels_[price - min_price]`, so finding a level is
  an add, not a tree walk.
- Each level is an intrusive doubly-linked list of `Node`s; `alignas(64)` with a
  40-byte `Order` plus two pointers gives exactly one cache line per resting
  order (`static_assert(sizeof(Node) == 64)`).
- The best price is found with a two-level bitmap and `ctz`/`clz` instead of
  `map.begin()`; empty-level transitions update the bitmap incrementally.
- An open-addressing hash table, pre-sized to never rehash, maps id → node.

The trade-offs are explicit: prices must lie in a fixed window and the book
capacity is fixed at construction. Both are checked, and rejection is counted.

### 4.2 Mutex ring buffer

A fixed circular array guarded by `std::mutex` with `not_full` / `not_empty`
condition variables. Producers block when full, the consumer blocks when empty.
This is the "OS does the scheduling" baseline: every empty→non-empty transition
requires a wakeup.

### 4.3 Lock-free SPSC ring buffer

One producer writes `tail_`, one consumer writes `head_`, so no variable is
written by both threads and no CAS loop is needed.

- `head_` and `tail_` are on separate cache lines (`alignas(64)`) to avoid false
  sharing.
- Each side caches the other side's index locally and re-reads the shared atomic
  only when the queue *looks* full or empty, which removes most cross-core
  traffic.
- Counters increase monotonically (never wrapped), so full/empty is a subtraction
  and no slot is wasted; the index is `counter & mask` with a power-of-two
  capacity.
- Ordering is the minimum required: producer stores the slot, then publishes with
  `release`; consumer loads with `acquire` before reading the slot, and vice
  versa on the return path.
- Waiting is a bounded spin with `_mm_pause`, followed by a yield **once per
  4096 spins**. Yielding on every failed iteration turns into a `sched_yield`
  storm; yielding never starves a peer that shares the core. The number of yields
  is exported as `g_yields` so busy-wait cost is visible in the results.

### 4.4 Slab allocator vs heap

`SlabAllocator<T, N>` pre-allocates one block, carves it into `sizeof(T)` chunks,
and threads free slots through an in-place free list stored in the slots
themselves, so bookkeeping costs no extra memory. Allocation pops; deallocation
pushes. `HeapAllocator<T>` is the control: identical book code, but each node
comes from aligned `operator new` / `operator delete`.

### 4.5 Affinity and scheduling

- `pin` mode pins **only the engine**, to CPU 2, with
  `pthread_setaffinity_np`. The network thread is deliberately left floating.
- `ME_PIN_NET=1` additionally pins the network thread (CPU 4) — used only as a
  probe (Section 6.11).
- `rt` mode attempts `SCHED_FIFO` at priority 50. On this host it fails with
  `EPERM` (`rtprio_limit=0`, no `CAP_SYS_NICE`). The harness records the failure
  in `results/rt-attempt.txt` (`exit_code=2`) rather than silently falling back.
- The client is pinned to CPU 6 by the harness so that client jitter is not
  mistaken for server jitter; the probe `probe-client-free` removes that pinning.

### 4.6 Network ingestion

Both servers implement `INetworkServer` and push into the same queue through the
same function pointer, so the queue and engine cannot tell them apart.

| | `epoll` | `io_uring` |
|---|---|---|
| Wait | `epoll_wait` on a listening + connected fd | `io_uring_wait_cqe_timeout` |
| Read | edge-triggered **drain loop**: `recv()` until `EAGAIN` | **single-shot** `recv` SQE, re-posted after each completion |
| Buffer | 4096 bytes per connection, partial messages carried over | same |
| Syscall shape | one wait amortized over many reads | one submit + one completion per read |

Both set `TCP_NODELAY` and parse with the same `wire::parse`, which only requires
that a message is fully buffered before it is consumed.

### 4.7 Measurement instrumentation

Every run produces one CSV row (42 columns) covering the latency distribution
(p50/p90/p95/p99/p99.9/max), the arrival/service split, per-thread CPU time,
context switches, minor/major faults, network syscall counts, SPSC yields, ingest
lateness, and run-queue wait. Run 1 of each configuration also writes the raw
per-order samples, which is what makes the cluster analysis in Section 6.5
possible.

Percentiles are used instead of averages throughout: an average can look
excellent while hiding a 14 ms stall, and every effect this project is about is a
tail effect.

---

## 5. Methodology

**Rule.** Change one variable at a time. All comparisons below hold the book,
queue, scheduling and network fixed except the one under test.

### 5.1 Workload and protocol

| Block | Shape | Repetitions |
|---|---|---|
| Integrated matrix | queue {mutex, spsc} × scheduling {default, pin} × network {epoll, io_uring} | 8 configs × 10 latency + 5 throughput |
| Allocator in the pipeline | fast-slab / fast-heap / V1 book | 3 × 10 latency + 5 throughput |
| Saturation sweep | queue {spsc, mutex} × offered rate {50 k … 6.4 M/s, unthrottled} | 2 × 9 × 3 |
| Components | matching core, queue+core, allocator isolation, false sharing | 4 × 9 |
| Probes | ingress attribution, client pinning, network pinning | 4 × 3 |

231 recorded pipeline runs plus 36 component repetitions. Each latency run sends
1 010 000 orders (10 000 discarded as warm-up); each throughput run 2 000 000.

**Latency runs.** One TCP client, deterministic seed 42, 64-order batches,
open loop at **200 000 orders/s** (the client paces itself, so a slow server
cannot slow the sender). Timestamp at client send; stop the clock when the engine
completes the order. Report the **median across repetitions** and keep every raw
run.

**Throughput runs.** Same client unthrottled (1024-order batches), 2 000 000
orders. This is what the queue and allocator cost when nobody is waiting.

**Saturation runs.** Fixed offered rate, 2 s measured window (shortened at high
rates to stay inside the 4 M-sample recorder), counting blocking `push`/`pop`
calls. This is the only block that exercises a full queue.

**Component floors (no network).** `bench_order_book` runs 2 000 000 mixed
operations (70 % new orders, 30 % cancels) against a single-threaded book.
`bench_pipeline` runs producer → queue → engine at 200 000 orders/s with the
producer on CPU 6 and the engine on CPU 2. `bench_allocator` times 2 000 000
alloc/free pairs on 64-byte nodes with a live set of 8192. `bench_false_sharing`
runs 5 000 000 orders through two byte-identical SPSC implementations that differ
only in index layout.

### 5.2 Measurement conditions

| Item | Value |
|---|---|
| Kernel | `7.2.9-arch1-1`, `PREEMPT_DYNAMIC` |
| CPU | 4 P-cores × 2 SMT (≤ 4.4 GHz) + 4 E-cores (≤ 3.1 GHz), 12 threads |
| Compiler | GCC 16.2.1, Release (`-O3 -DNDEBUG`), rebuilt between phases, no sanitizers |
| liburing | 2.15 |
| Session | TTY login, no graphical session, no browser or editor |
| `loadavg` at start | 0.21 / 0.05 / 0.02 |
| Platform profile | `performance` (`EPP=performance` on all 12 CPUs), on AC |
| Governor | `powersave` (intel_pstate), turbo enabled, SMT enabled |
| `SCHED_FIFO` | **denied** (`EPERM`, `rtprio_limit=0`) |
| `perf` | **unavailable** (binary missing, `perf_event_paranoid=2`) |
| `strace` | unavailable |
| `isolcpus` / `nohz_full` | not available (requires root) |

The platform profile pins the energy-performance preference, but `intel_pstate`
still owns the frequency, so clocks move between 0.8 GHz at idle and 4.4 GHz
under load. The measurements are therefore a fair A/B under identical conditions,
not an absolute latency floor.

**Thermal and frequency evidence.** A 5-second sampler recorded package
temperature and the fastest P-core frequency for the whole session
(`results/thermal-<ts>.csv`, 199 samples over 16 minutes):

| Metric | Value |
|---|---|
| Package temperature | min 47 °C, median 55 °C, max **71 °C** |
| Fastest P-core frequency | median 4.37 GHz, max 4.42 GHz |
| Samples at ≥ 95 °C | none |
| Samples below 4.0 GHz | 5 of 199, all between benchmark runs or during the single-threaded book bench |

There was no thermal throttling, so the matrix ordering (SPSC configurations
first, mutex later) does not bias the queue comparison through a drifting clock.

### 5.3 Instrumentation definitions

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
| **clean run** | no order was already > 1 ms old at parse |

A **clean run** is the measurement-quality gate. Delayed runs are recorded rather
than deleted, so the matrix stays auditable; configurations are ranked on clean
runs only, and a configuration with no clean runs is reported as "not measurable
on this host" rather than as evidence about the code.

### 5.4 Statistics

Differences are tested with a two-sided permutation test on the difference of
medians (20 000 resamples, clean runs only). With n = 9–10 per configuration this
resolves differences of roughly 30 % or more; smaller differences are reported as
"not distinguishable". Every claim in Section 7 either has a test behind it or is
labelled as an observation about a single host.

### 5.5 What could not be done, and what we did instead

| Planned control | Status | Consequence |
|---|---|---|
| `performance` governor, turbo off | no permission | CPU frequency is not fixed; p50 values carry frequency variance |
| `isolcpus` for the engine core | no permission | "pinned" ≠ "isolated"; this becomes a *finding*, Section 6.6 |
| `SCHED_FIFO` | denied (`EPERM`) | realtime arm is a documented null result |
| `perf stat` / `perf record` | unavailable | mechanism evidence comes from in-process counters (`schedstat`, `getrusage`, `NetStats`) instead of PMU events |
| fully controlled host | not available | host sensitivity is measured as a controlled variable (Section 6.12) instead of being assumed away |

---

## 6. Results

All latency numbers are **client send → engine complete** on the same host, in
microseconds unless stated. Values are medians across repetitions; the p99.9
column uses only clean runs where the two differ, and the number of clean runs is
always printed. "Not distinguishable" means the permutation test returned
p > 0.05.

### 6.1 Component floors (no network)

`bench_order_book`, 2 000 000 ops, single thread, engine on CPU 2:

| book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| V1 `map`+`list`+`unordered_map` | 11.63 M/s | 81 ns | 420 ns | 595 ns | **10.97 ms** |
| V3 array+slab+bitmap | **33.68 M/s** | 94 ns | 247 ns | 374 ns | **26.7 µs** |

V3 is **2.9× the throughput** but is *not* faster at the median (94 ns vs 81 ns).
Its contribution is the tail: V3's worst case is **26.7 µs** against V1's
**10.97 ms**, a factor of 411. A median-only report would have missed the entire
point of the data structure. Both books produce identical trade counts
(1 093 640 trades, 88 304 cancels hit, 512 136 miss).

`bench_pipeline`, 1 010 000 orders at 200 000 orders/s, producer CPU 6, engine
CPU 2:

| queue | book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| spsc | V3 | **7.35 M/s** | **299 ns** | 1.6 µs | **13.2 µs** | 160 µs |
| spsc | V1 | 5.77 M/s | 247 ns | 836 ns | 5.40 ms | 10.24 ms |
| mutex | V3 | 3.49 M/s | 1.6 µs | 3.9 µs | 14.6 µs | 404 µs |
| mutex | V1 | 3.27 M/s | 1.6 µs | 3.6 µs | 6.26 ms | 11.02 ms |

This is the cleanest view of both OS mechanisms in the project, because no TCP is
involved. SPSC removes the wakeup: for the same book, p50 drops from 1.6 µs to
299 ns (5.4×) and throughput rises 2.1×. V3 removes the allocator stall: with
either queue, V1's p99.9 is 5.4–6.3 ms and V3's is 13–15 µs. All four
configurations produced identical trade counts and quantities (789 497 trades,
20 135 763 traded quantity), so the speed did not come from matching less.

### 6.2 Integrated latency matrix

200 000 orders/s, 1 010 000 orders, 10 000 warm-up, n = 10 per configuration.

| config | clean | p50 | p95 | p99 | p99.9 | p99.9 (clean) |
|---|---|---|---|---|---|---|
| spsc / pin / epoll | 9/10 | 25.0 µs | 34.5 µs | **37.6 µs** | 85.6 µs | **86.8 µs** |
| spsc / pin / uring | 10/10 | 24.8 µs | 33.6 µs | 36.7 µs | 96.4 µs | 96.4 µs |
| spsc / default / epoll | 10/10 | 26.8 µs | 36.2 µs | 38.9 µs | 127.7 µs | 127.7 µs |
| spsc / default / uring | 10/10 | **24.2 µs** | 31.1 µs | 36.0 µs | 121.7 µs | 121.7 µs |
| mutex / default / epoll | 10/10 | 24.3 µs | 64.7 µs | 71.7 µs | 158.5 µs | 158.5 µs |
| mutex / default / uring | 10/10 | 23.8 µs | 60.3 µs | 69.7 µs | 150.6 µs | 150.6 µs |
| mutex / pin / epoll | 10/10 | 27.2 µs | 64.9 µs | 70.9 µs | 105.3 µs | 105.3 µs |
| mutex / pin / uring | 10/10 | 29.1 µs | 66.0 µs | 71.1 µs | 113.7 µs | 113.7 µs |
| allocator: fast-slab | 10/10 | 26.6 µs | 34.9 µs | 37.5 µs | 98.7 µs | 98.7 µs |
| allocator: fast-heap | 10/10 | 24.5 µs | 32.9 µs | 35.9 µs | 119.5 µs | 119.5 µs |
| allocator: V1 book | 10/10 | 24.4 µs | 32.3 µs | 36.6 µs | **6.52 ms** | 6.52 ms |

Reading the table:

- Every configuration sits at **p50 24–29 µs**. The spread across queue, network
  and scheduler choices is 5 µs, i.e. smaller than the offset between the median
  and the 95th percentile of any mutex configuration.
- The queue, not the network, decides the shape above p95. SPSC climbs from
  34 µs to 38 µs between p95 and p99 (a 3 µs step); mutex climbs from 65 µs to
  71 µs (a 30 µs step between p50 and p95). Two distinct distributions with the
  same centre.
- The V1 book is the only row whose p99.9 is three orders of magnitude above its
  median, and Section 6.5 shows that stall is inside the engine.
- 120 of 122 latency runs were clean. The two exceptions are single startup
  episodes, recorded with their order counts in `results/summary.md`: 64 orders
  in the first run of the first configuration (max arrival 2.17 ms, network
  run-queue wait 0.0 ms — the client was descheduled before `send`) and 192
  orders in `probe-net-pinned` run 3.

### 6.3 Where the tail lives: arrival vs service

Same runs, medians. *Arrival* = client send → network-thread parse. *Service* =
parse → engine completion (queue wait + matching). "Share" is arrival p99.9 as a
fraction of total p99.9.

| config | p50 total | arrival p50 | service p50 | p99.9 total | arrival p99.9 | service p99.9 | arrival share |
|---|---|---|---|---|---|---|---|
| spsc / pin / epoll | 25.0 µs | 21.6 µs | 3.1 µs | 85.6 µs | 43.9 µs | 23.6 µs | 51 % |
| spsc / pin / uring | 24.8 µs | 21.7 µs | 3.0 µs | 96.4 µs | 71.2 µs | **16.1 µs** | 74 % |
| spsc / default / epoll | 26.8 µs | 23.7 µs | 3.1 µs | 127.7 µs | 83.8 µs | 20.7 µs | 66 % |
| spsc / default / uring | 24.2 µs | 21.2 µs | 3.1 µs | 121.7 µs | 86.4 µs | 27.1 µs | 71 % |
| mutex / pin / epoll | 27.2 µs | 16.2 µs | 10.8 µs | 105.3 µs | 58.7 µs | 42.0 µs | 56 % |
| mutex / pin / uring | 29.1 µs | 19.9 µs | 10.4 µs | 113.7 µs | 60.9 µs | 42.3 µs | 54 % |
| mutex / default / epoll | 24.3 µs | 16.8 µs | 6.4 µs | 158.5 µs | 60.2 µs | **102.6 µs** | 38 % |
| mutex / default / uring | 23.8 µs | 17.3 µs | 6.1 µs | 150.6 µs | 69.2 µs | 95.0 µs | 46 % |
| allocator: V1 book | 24.4 µs | 21.1 µs | 2.7 µs | 6.52 ms | 47.3 µs | **6.50 ms** | 1 % |

This table is the core result:

- For every SPSC configuration, arrival accounts for **51–74 %** of p99.9 and the
  engine-side service component is 16–27 µs. The component pipeline with no
  network at all has p99.9 of **13.2 µs**, the same order of magnitude — so the
  residual engine-side tail is the polling loop's phase (an order pushed just
  after the consumer last checked is picked up on the next pass) plus sharing a
  machine with the network thread, not matching cost.
- The mutex rows invert part of the ratio: arrival is *lower* in absolute terms
  (59–69 µs) because its consumer is asleep and the network thread runs
  undisturbed, but service rises to 42–103 µs. Median over the four
  configurations: **service p99.9 20.7 µs (SPSC) vs 68.6 µs (mutex), 3.3×**.
- The V1 row inverts the ratio completely: arrival is normal (47 µs), service is
  6.50 ms, so 99.7 % of the tail is the book.
- At the median, SPSC service is 3.0–3.1 µs. The integrated median of ~25 µs is
  therefore **86–88 % ingress and wake-up**.

### 6.4 Queue: mutex vs lock-free SPSC

Fixed: fast-slab book, epoll, default scheduling, 200 000 orders/s.

| metric | mutex | SPSC | ratio |
|---|---|---|---|
| p50 | 24.3 µs | 26.8 µs | 0.9× (p = 0.14, noise) |
| p99 | **71.7 µs** | **38.9 µs** | **1.8×** (p = 0.0009) |
| service p99.9 | 102.6 µs | 20.7 µs | 5.0× |
| throughput (unthrottled) | 3.19 M/s | 5.29 M/s | 1.7× |
| engine CPU per order | 0.32 µs | 0.19 µs | 1.7× |
| network thread voluntary switches | 159 756 | 15 | ~10 000× |
| network thread involuntary switches | 6 | 4 | – |

The queue comparison is statistically resolvable at **p99 and at service p99.9,
and not at p50**:

| comparison (clean runs) | SPSC | mutex | p |
|---|---|---|---|
| p50, default / epoll | 26.8 µs | 24.3 µs | 0.14 — not distinguishable |
| p99, default / epoll | 38.9 µs | 71.7 µs | **0.0009** |
| p99, pin / epoll | 38.3 µs | 70.9 µs | **0.0017** |
| service p99.9, pin / epoll | 20.7 µs | 42.0 µs | **0.0007** |
| total p99.9, pin / epoll | 86.8 µs | 105.3 µs | 0.14 — not distinguishable |

**Mechanism.** At p50 the two queues are the same because at 200 000 orders/s
the queue is almost never empty; both hand off in nanoseconds. The difference
appears where the consumer has to wait. The mutex queue parks the consumer on a
condition variable, so each batch costs a wakeup: the producer enters the kernel,
a CPU is chosen for the consumer, and the consumer resumes with a cold pipeline.
That cost appears as 157 000–199 000 voluntary switches in the *producer* and
102.6 µs of *service* tail — not as arrival tail, which is why the arrival share
is lower for mutex. The SPSC consumer never sleeps: it spins, sees `tail_` change
within a few hundred nanoseconds, and proceeds.

The throughput rows confirm the same mechanism from the other direction: 5.29 vs
3.19 M/s and 0.19 vs 0.32 µs of engine CPU per order. The SPSC buffer does its own
waiting instead of asking the kernel to do it.

Caveat: the mutex p50 is *lower* here, and it was higher in the pinned
configuration (27.2 vs 25.0 µs), so p50 is treated as a tie; the conclusion rests
on the p99, the service tail and throughput, which differ consistently and
significantly.

### 6.5 Allocator: slab vs heap vs the V1 book

**In isolation** (`bench_allocator`, 2 000 000 ops, live set 8192, 9 reps):

| allocator | alloc ns/op | alloc p99.9 | alloc sd | free ns/op | free p99.9 | free sd |
|---|---|---|---|---|---|---|
| aligned `::operator new/delete` | 17.14 | 44 ns | 27.5 | 22.19 | 136 ns | 86.2 |
| `SlabAllocator` | **12.59** | **16 ns** | **6.9** | **14.13** | **24 ns** | **8.5** |

1.36× faster to allocate, 1.57× to release, with roughly a quarter of the
variance. Absolute values include ~20–25 ns of clock cost in both variants, so
the ratio and the spread are the meaningful numbers.

**In the pipeline** (fixed: SPSC, epoll, pinned, 200 000 orders/s):

| book / allocator | p50 | p99.9 | service p99.9 | max | throughput |
|---|---|---|---|---|---|
| V3 + slab | 26.6 µs | 98.7 µs | 13.1 µs | 265 µs | 5.50 M/s |
| V3 + `operator new` | 24.5 µs | 119.5 µs | 27.9 µs | 348 µs | 5.08 M/s |
| V1 map/list/malloc | 24.4 µs | 6.52 ms | 6.50 ms | 11.15 ms | 4.32 M/s |

The honest result is that **slab and heap are not distinguishable end-to-end**:
98.7 vs 119.5 µs p99.9, permutation p = 0.38, with run ranges of 57.7–147.5 and
67.9–150.5 µs. At 200 000 orders/s allocation is off the critical path — the
~25 µs of ingress and wake-up covers it. A fixed 64-byte `new`/`delete` pair is
served by glibc's per-thread cache in tens of nanoseconds, and the component
bench shows the slab's advantage where it actually exists: the tail of the
distribution and its variance, not the mean.

The V1 row is the real allocator/data-structure lesson. Its service p99.9 is
**6.50 ms**, arrival is normal (47 µs), and the run-to-run p99.9 range is
6.14–7.05 ms — a reproducible stall, not a random outlier. In the raw per-order
samples the slow orders arrive in clusters of arbitrary size (431, 1046, 2738
orders) starting at arbitrary points in the run, whereas host-delay clusters are
built from whole 64-order client batches. That is the signature of a *book-side*
stall.

Mechanism: `std::unordered_map` grows by rehashing, and each rehash reallocates
and relinks every bucket; `std::map` and `std::list` allocate one node per
operation from the general heap. The V3+heap control rules out `operator new`
alone as the cause: same allocator call, 55× smaller p99.9, because the sizes are
fixed and the containers never rehash.

### 6.6 Scheduling: affinity without isolation

Fixed: SPSC and mutex, fast-slab, epoll. `default` = nothing pinned; `pin` =
engine on CPU 2.

| config | clean | p50 | p99 | p99.9 | engine run-queue wait | engine involuntary switches |
|---|---|---|---|---|---|---|
| spsc / default | 10/10 | 26.8 µs | 38.9 µs | 127.7 µs | 2.20 ms | 27 |
| spsc / pin | 9/10 | 25.0 µs | 37.6 µs | 85.6 µs | **0.22 ms** | 26 |
| mutex / default | 10/10 | 24.3 µs | 71.7 µs | 158.5 µs | 0.06 ms | 2 |
| mutex / pin | 10/10 | 27.2 µs | 70.9 µs | 105.3 µs | 72.6 ms | 0 |

**Mechanism.** Pinning the engine to CPU 2 does not give it CPU 2. Without
`isolcpus` (and without realtime priority, which the host denies), ordinary work
is still scheduled on that core, and a pinned thread cannot migrate away from it.

The run-queue numbers show exactly that, and they behave differently for the two
queue types:

- With the **spinning** (SPSC) queue, the engine is already runnable when the
  order arrives, so its accumulated run-queue wait is small (2.20 ms unpinned,
  0.22 ms pinned) and pinning does what it is designed to do.
- With the **blocking** (mutex) queue, the engine is asleep between batches.
  Every wakeup is a *new* scheduling decision, and `schedstat` counts the time a
  woken thread waits to be placed on a CPU — which the spinning consumer never
  accrues and the sleeping one always does. The pinned mutex row's 72.6 ms
  (range 0.08–154 ms) is the accumulation of ~17 000 voluntary sleeps over the
  run, not a stall: its median latency stays at 27 µs.

Pinning improved the p99.9 from 127.7 to 86.8 µs, a 32 % reduction that does
**not** reach significance at n = 10 (p = 0.14), because the network thread still
floats and it is the component that absorbs host interference. The expected
benefit requires `isolcpus`/cgroup cpuset isolation, which we could not apply.

`SCHED_FIFO` at priority 50 was attempted and denied (`EPERM`); `rtprio_limit=0`
and the binary has no capabilities. `results/rt-attempt.txt` records the failure
and the error text. This arm is a null result by host policy, not by design.

### 6.7 Network: epoll vs io_uring

Fixed: SPSC, fast-slab, pinned. Latency at 200 000 orders/s; kernel operations
counted per run.

| config | p50 | p99 | p99.9 | kernel ops (paced, 1.01 M orders) | kernel ops (unthrottled, 2 M orders) | throughput |
|---|---|---|---|---|---|---|
| epoll | 25.0 µs | 37.6 µs | 85.6 µs | 47 346 | 20 170 | 5.86 M/s |
| io_uring | 24.8 µs | 36.7 µs | 96.4 µs | 31 569 | 40 328 | 5.48 M/s |

Latency and throughput are statistically indistinguishable; the p99.9 ordering
(86 vs 96 µs) reverses between the pinned and unpinned pairs, so no latency
winner is claimed. The mechanism difference is in system calls, and it **inverts**
between the two load regimes:

- **Paced (producer slower than consumer).** The client sends 64-order batches
  every 320 µs. io_uring needs one recv SQE and one completion per batch
  (≈ 15.8 k × 2 ≈ 31.6 k ops). epoll needs one `epoll_wait` *plus* at least one
  `recv` that returns data and one that returns `EAGAIN` to detect the drain
  (≈ 15.8 k waits + 31.6 k reads ≈ 47.3 k ops). io_uring needs 33 % fewer
  operations.
- **Unthrottled (consumer can never catch up).** The socket buffer is always
  non-empty, so epoll's drain loop services many `recv`s per readiness event and
  waits only when it finally catches up (≈ 20.2 k ops for 2 M orders). io_uring's
  single-shot recv re-posts one SQE per read (≈ 19.6 k reads → ≈ 40.3 k ops).
  epoll needs 50 % fewer.

That is the answer to "when does io_uring help": it removes the per-event wait
when traffic is bursty and the reader is faster than the writer; it does not help
— and costs more submissions — when the reader is saturated, because it has no
drain loop to amortize over. Neither difference moved a latency percentile here,
because at 200 000 orders/s the dominant cost is not syscall count.

### 6.8 Throughput and CPU accounting

Unthrottled client, 2 000 000 orders, 5 repetitions each.

| config | orders/s | engine CPU/order | net CPU/order | net `nvcsw` | kernel ops/run |
|---|---|---|---|---|---|
| spsc / pin / epoll | **5.86 M/s** | **0.17 µs** | 0.17 µs | 12 | 20 190 |
| spsc / pin / uring | 5.48 M/s | 0.18 µs | 0.18 µs | 12 | 40 328 |
| spsc / default / epoll | 5.29 M/s | 0.19 µs | 0.19 µs | 15 | 20 194 |
| spsc / default / uring | 5.14 M/s | 0.19 µs | 0.19 µs | 15 | 40 328 |
| mutex / pin / uring | 3.38 M/s | 0.30 µs | 0.21 µs | **157 151** | 40 328 |
| mutex / pin / epoll | 3.34 M/s | 0.30 µs | 0.20 µs | 172 731 | 20 170 |
| mutex / default / epoll | 3.19 M/s | 0.32 µs | 0.20 µs | 159 756 | 20 168 |
| mutex / default / uring | 3.12 M/s | 0.32 µs | 0.20 µs | 198 511 | 40 328 |

The network thread costs 0.17–0.21 µs per order in every configuration, which is
the control that says the queue is what changed. Engine CPU per order is
0.17–0.19 µs for SPSC and 0.30–0.32 µs for mutex.

**Ceiling caveat.** These "unthrottled" figures are *client-limited*. The
unthrottled client sends 1024-order batches, blocks on a full socket, and
delivered 5.5–5.9 M orders/s, so the SPSC numbers here are a harness limit, not a
server limit. The paced 6.4 M/s client sends 64-order batches and never blocks;
Section 6.9 shows the SPSC pipeline absorbing that rate with zero full-queue
waits, so the server ceiling is **above 6.4 M/s**. The mutex numbers are a
genuine service result.

**CPU accounting.** At 200 000 orders/s the SPSC engine reports ~101 % CPU — one
core fully occupied, mostly spinning — while the mutex engine reports 8–9 %
because it sleeps on a condition variable. This is the clearest OS-level
trade-off in the project: the lock-free queue buys tail latency and throughput by
keeping a core busy. The mutex baseline is power-friendly and equally fast at the
median, but it pays a scheduler round trip on every hand-off and stops absorbing
bursts well before 6.4 M orders/s.

### 6.9 Saturation: where each queue stops absorbing

Offered rate is what the client paced; achieved is what the server completed.
Each cell is the median of 3 runs.

| offered | SPSC achieved | SPSC p50 | SPSC full waits | mutex achieved | mutex p50 | mutex full waits |
|---|---|---|---|---|---|---|
| 50 k/s | 50 k/s | 112.1 µs | 0 | 50 k/s | 308.0 µs | 0 |
| 100 k/s | 100 k/s | 23.1 µs | 0 | 100 k/s | 64.6 µs | 0 |
| 200 k/s | 200 k/s | 22.8 µs | 0 | 200 k/s | 25.6 µs | 0 |
| 400 k/s | 400 k/s | 11.7 µs | 0 | 400 k/s | 15.0 µs | 0 |
| 800 k/s | 800 k/s | 12.3 µs | 0 | 800 k/s | 14.3 µs | 0 |
| 1.6 M/s | 1.6 M/s | 10.5 µs | 0 | 1.6 M/s | 13.7 µs | 0 |
| 3.2 M/s | 3.2 M/s | 10.2 µs | 0 | 3.2 M/s | 13.5 µs | 0 |
| 6.4 M/s | **6.40 M/s** | 10.3 µs | **0** | 3.32 M/s (2 of 3 runs) | 32.7 ms | 414k–495k |
| unthrottled | 5.56 M/s | 12.9 ms | 1 841 316 | 3.33 M/s | 21.5 ms | 250k |

Four findings:

1. **The SPSC queue is not close to saturation.** It returned 6.40 M/s in 3 of 3
   runs with **zero** full-queue waits; the harness cannot offer more. The cost is
   on the other side: 43 158 of 3 000 000 pops (1.4 %) found the ring empty and
   had to spin. At the unthrottled closed-loop rate the queue *does* fill
   (1.84 M full waits), because a 1024-order batch arrives faster than 64 bytes
   per order can be matched — that row measures client-blocked response time, not
   queue capacity.
2. **The mutex path is bistable at 6.4 M/s, not simply capped.** Two of three runs
   collapsed to ~3.3 M/s with a 32.7 ms median, 414k–495k blocking pushes and
   208k–259k voluntary switches in the network thread. The third run kept up:
   6.01 M/s, 0.62 ms median, 26 654 full waits — but still with a **30.9 ms worst
   case** at the end of the run. With n = 3 this is a warning, not an estimate:
   the stable ceiling is 3.2 M/s (zero waits), the collapse point is below
   6.4 M/s, and *whether* it collapses appears to depend on which sleep/wake
   pattern the two threads fall into.
3. **A closed-loop throughput number alone would hide this.** The unthrottled
   client blocks on the socket, so the mutex pipeline settles at 3.3 M/s in 3 of
   3 runs; the open-loop sweep shows the same code reaching 6.0 M/s when it
   happens not to collapse. The two measurements bound the behaviour; neither is
   the whole answer.
4. **Latency is not monotonic in offered rate.** Both queues get *faster* as the
   rate rises from 50 k/s to ~3.2 M/s (mutex 308 → 13.5 µs, SPSC 112 → 10.2 µs)
   because wake-up cost is amortised over bigger batches. Below ~200 k/s the
   blocking queue is 2.7× slower than the spinning one at the median. The idle
   case is where the mutex loses most, not the busy case — the opposite of the
   usual "locks are fine at low load" intuition, because at low load every order
   pays a full sleep/wake.

At the normal 200 000 orders/s rate the blocking counters are: zero full waits
for every configuration, 15 792–15 822 empty waits per run for SPSC, and
18 498–33 164 for mutex (it finds the queue empty *and* sleeps, which is why its
empty count is higher despite identical traffic).

### 6.10 False sharing on the SPSC boundary

`bench_false_sharing`, 5 000 000 orders per rep, 9 reps, producer CPU 6,
consumer CPU 2. Two buffers with byte-identical push/pop logic differ only in
layout: `padded` gives each ownership domain its own cache line (what
`include/spsc_ring_buffer.hpp` does), `packed` puts all four indices adjacent.

| layout | throughput median | throughput best | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| padded | 8.59 M/s | 9.66 M/s | **153 ns** | 244 ns | 17.8 µs | 70.3 µs |
| packed | **10.28 M/s** | **11.78 M/s** | 197 ns | 530 ns | 26.2 µs | 71.8 µs |

**The honest reading is a trade-off, not a win.** Per-item latency favours
padding in 9 of 9 reps here and 9 of 9 in the independently run contended
repetition of the same experiment — 18 of 18 overall, two-sided sign test
p ≈ 8 × 10⁻⁶. Throughput moves the other way in 8 of 9 reps here
(0.75–1.09× padded/packed), while the contended repetition had padding ahead in
3 of 9 (0.41–2.12×) and this one in 1 of 9. The throughput direction is not
stable across host conditions, so no throughput claim is made.

The plausible mechanism — not verified here — is that packing lets the slot write
and the index publish land on the same line, which reduces the number of line
transfers per item (higher rate) but puts both threads on one line (higher
per-item latency). The production queue uses the padded layout *and* caches the
peer index locally, so the remote line is touched only when the queue looks empty
or full; this benchmark does not measure that path, so it cannot be used to
justify the production layout. Padding is kept because the mechanism is sound and
the alternative has no demonstrated advantage, not because this experiment proves
it.

### 6.11 Probes: attributing the residual tail

All probes use SPSC, fast-slab, pinned engine, 200 000 orders/s,
`ME_INGEST_STATS=1`, 3 reps.

| probe | p50 | p99 | p99.9 | orders > 1 ms before parse | max arrival delay |
|---|---|---|---|---|---|
| baseline (`probe-ingest-epoll`) | 24.6 µs | 38.0 µs | 51.5 µs | 0 / 1 000 000 | 293 µs |
| client **not** pinned (`probe-client-free`) | 23.0 µs | 31.1 µs | 63.8 µs | 0 / 1 000 000 | 324 µs |
| `io_uring` baseline | 24.3 µs | 35.1 µs | 113.3 µs | 0 / 1 000 000 | 167 µs |
| network thread pinned to CPU 4 | 23.1 µs | 30.7 µs | 127.5 µs | 0 / 1 000 000 | 988 µs |

Two conclusions:

- **Client pinning is not the source of the tail.** With the client free to
  migrate, p99.9 is 63.8 µs against 51.5 µs pinned, and no order is late. Pinning
  the client is therefore a legitimate control, not a way of hiding client
  jitter.
- **Pinning the network thread does not help.** It raises p99.9 from 51.5 to
  127.5 µs and its worst arrival delay from 293 µs to 988 µs; one of its three
  runs contained 192 late orders (max 1.67 ms). This is the same lesson as
  Section 6.6 measured on the producer side: a thread confined to a core it does
  not own loses the ability to migrate away from unrelated work.

### 6.12 Host sensitivity: how much of the tail is the machine

The host is part of the measurement system, so it was measured as a variable
rather than assumed away. The same binary and configuration
(`spsc / pin / epoll`) was run under three host conditions:

| condition | p50 | p99 | p99.9 | network run-queue wait | orders > 1 ms |
|---|---|---|---|---|---|
| competing desktop process free on all 12 CPUs (n = 3) | 26.0 µs | 1046 µs | **2400 µs** | 97.7 ms | 10 017 |
| that process confined to the E-cores (n = 3) | 22.5 µs | 68 µs | **142 µs** | 1.2 ms | 0 |
| quiet host, `platform_profile=performance` (matrix row, 9/10 clean) | 25.2 µs | 37.6 µs | **86.8 µs** | 0.5 ms | 0 |

The confinement half of the first experiment used a shell `trap` to restore
affinity; no process was killed, reniced or stopped, and both halves ran while
the competing process was still at ~380 % CPU. The engine's own run-queue wait in
the confined half was 0.29 ms, so the engine was not the delayed component. A
mutex-queue control measured in the same confined window gave p99.9 = 299 µs,
consistent with the matrix's uncontended mutex rows, which shows the improvement
was not an artefact of the queue type. Full transcript in
`results/host-isolation.txt`.

Three things follow, and they are the reason the numbers in Section 6 can be
quoted:

1. **The multi-millisecond tail is host ingress delay.** Confining the competing
   process moved p99.9 by 17× without touching the code, and the per-run
   fingerprint is unambiguous: `net_ingest_slow` in the thousands, network
   run-queue wait in the tens of milliseconds, arrival p99.9 ≈ 2.4 ms, service
   p99.9 unchanged.
2. **The ordering is a property of the code, not the machine.** Every ordering
   that matters survives the change of host condition: SPSC > mutex on
   throughput (1.58× under contention, 1.75× on the quiet host), V3 > V1 on the
   tail, slab > heap in isolation, padding > packing on per-item latency.
   Absolute throughput moved 6–24 % upward because pinned configurations were no
   longer losing their cores to unrelated work.
3. **The median is insensitive to host contention; the tail is not.** Median
   latency across the eight configurations was 22.6–33.0 µs under contention and
   23.8–29.1 µs on the quiet host. Removing the contention did not make a
   wake-up cheaper; it removed the multi-millisecond delays that dominate
   p99.9. This is also why contention is measured per run: the clean-run gate
   turns "the host was noisy" from an excuse into a column.

All headline numbers in this report are from the quiet-host rows, and the
contended numbers are quoted only to quantify sensitivity. A configuration with
zero clean runs would be reported as "not measurable" rather than as evidence.

---

## 7. Answering the Central Question

**1. Where does latency go?** On loopback TCP, it goes to ingress, not to
matching. The optimized book needs 94 ns (p50) and 374 ns (p99.9); the queue adds
roughly 200 ns; the network path adds ~22 µs at p50 and 44–86 µs at p99.9. The
pipeline floor without a network is 299 ns / 13.2 µs, and the integrated median is
86–88 % ingress and wake-up. Any optimization aimed at the matcher would have
been invisible.

**2. Does synchronization matter?** Yes, at the point where a thread has to wait,
and not at p50. Replacing a condvar queue with an SPSC ring cuts p99 by 1.8×,
service p99.9 by 3.3× and raises throughput 1.75×, because it replaces a kernel
wakeup with a spin. The 157 000–199 000 → 12–15 voluntary switches are the
mechanism, not a correlate. It also changes the failure mode at overload from
"bistable collapse with 33 ms medians" to "absorbs everything offered".

**3. Does the allocator matter?** Only when the allocation pattern is irregular.
A fixed-size `new` is as good as a slab in this workload (p = 0.38 end-to-end);
in isolation the slab is 1.36–1.57× faster with a quarter of the variance. The
6.5 ms V1 tail comes from growing, pointer-chasing containers (rehash + per-node
allocation), and the V3+heap control isolates the cause.

**4. Does affinity help?** Not without isolation. Pinning cut the engine's
run-queue wait 10× but improved p99.9 by 32 % with p = 0.14 — a real mechanism
without a significant end-to-end effect, because the network thread still floats
and absorbs the interference. Pinning the *network* thread made p99.9 2.5× worse.
Realtime priority would have changed the picture, but it was unavailable.

**5. Is `io_uring` faster?** Not in latency or throughput here. Its benefit is
syscall count under bursty, reader-faster-than-writer traffic (47.3 k → 31.6 k
ops), and it reverses under saturation (20.2 k → 40.3 k). "Fewer syscalls" is not
the same as "lower latency".

**6. Does the design meet its goals?** Yes on the goals the project set, with one
goal only partly met. The goal was an instrumented engine in which each OS
mechanism could be isolated and explained. Every comparison is single-variable,
every conclusion has a counter behind it, and all configurations are numerically
identical. The partly met goal is the scheduling arm: pinning without
`isolcpus`, `SCHED_FIFO` and a fixed governor cannot demonstrate the realtime
benefit, so that arm is a documented null result rather than a measurement.

---

## 8. Limitations and Threats to Validity

- **One host, one kernel, one compiler.** Every number is conditional on
  `7.2.9-arch1-1`, GCC 16.2.1 and this laptop's P/E-core topology. Ratios are
  more portable than absolutes.
- **Frequency still varies.** `powersave` and `intel_pstate` keep the governor in
  charge; clocks held 4.2–4.4 GHz during runs but idle at 0.8 GHz between them.
  Run-to-run variance of ±5 % in throughput is consistent with this.
- **SMT is on.** The engine shares a physical core with its sibling; a neighbour
  on that sibling would roughly halve its throughput. This was not controlled and
  not observed.
- **No PMU counters.** `perf` was unavailable, so cache misses, branch misses and
  IPC are absent. Mechanism claims rest on in-process counters (`schedstat`,
  `getrusage`, syscall counts) plus the A/B controls, not on hardware events.
- **Loopback only.** Client and server share the host, so there is no NIC, no
  interrupt affinity and no kernel bypass. The arrival component would look
  different on real hardware, and probably larger.
- **Client-limited throughput.** The unthrottled client cannot drive the SPSC path
  past ~5.9 M/s, so the 6.4 M/s paced sweep is the only evidence about the
  ceiling, and it is a lower bound.
- **Small n for saturation.** 3 repetitions cannot characterize a bistable
  system; the mutex collapse probability is unknown.
- **The arrival component includes client-side work.** The load generator stamps
  a batch once, after its pacing wait but before it generates the 64 messages and
  calls `send`. Arrival therefore contains a few microseconds of client message
  generation plus the syscall, and it is quantized to batch boundaries (which is
  also why host-delay clusters appear as whole 64-order batches). This affects
  the arrival number, not the service number, and it cannot change the direction
  of the arrival/service attribution.
- **Percentile sums are approximate.** Total p99.9 is reported separately from
  arrival + service p99.9, not derived from it.
- **Nine to ten repetitions per configuration**, not hundreds. Medians are
  reported with the full p99.9 range so the reader can see the spread, and
  differences smaller than ~30 % are reported as not distinguishable.

---

## 9. Reproducing the Results

### 9.1 Where the data lives

| Path | Contents |
|---|---|
| `results/summary.md` | all generated integrated tables (Sections 6.2–6.9) |
| `results/summary.csv` | one row per config/kind, medians |
| `results/analysis.json` | machine-readable groups, flags, checksums |
| `results/components-summary.md` | component medians (Sections 6.1, 6.5, 6.10) |
| `results/components/*.txt` | raw component runs, 9 reps each |
| `results/env.txt` | host, power profile, EPP/frequency, thermal snapshot, commit |
| `results/host-isolation.txt` | the confinement experiment (Section 6.12) |
| `results/rt-attempt.txt` | the `SCHED_FIFO` denial |
| `results/bench-session-<ts>.log` | full transcript of a session (gitignored) |
| `results/thermal-<ts>.csv` | 5-second temperature/frequency samples (gitignored) |
| `results/raw/<config>/runs.csv` | every individual run (gitignored) |

### 9.2 One command

One command reproduces the entire measurement: environment capture, Release
rebuild, all five measurement phases, both analyses and a thermal sampler.

```bash
./scripts/run_session.sh
```

It refuses to start on a dirty tree, sets the platform profile to `performance`,
preserves any previous session's data instead of deleting it, prints each phase's
exit code, and writes a transcript to `results/bench-session-<ts>.log`. It takes
about 16 minutes and should be left alone while it runs.

The individual phases remain available if only one is wanted:

```bash
# build and test
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
ctest --test-dir build-rel --output-on-failure     # 7/7

# full verification: Release + ASan/UBSan + TSan + epoll/io_uring parity
./scripts/verify.sh

# measurement
./scripts/env.sh                                   # results/env.txt
./scripts/run_matrix.sh                            # 8 configs × 10 latency + 5 throughput
./scripts/run_experiments.sh                       # allocator variants + SCHED_FIFO probe
./scripts/run_saturation.sh                        # 2 queues × 9 offered rates × 3
./scripts/run_components.sh                        # book, pipeline, allocator, false sharing
./scripts/run_probe.sh                             # ingress, client and network pinning
python3 ./scripts/analyze.py                       # results/summary.md, .csv, .json, plots
python3 ./scripts/analyze_components.py            # results/components-summary.md
```

A single measurement can be repeated with
`scripts/run_one.sh CONFIG QUEUE BOOK NET SCHED KIND RUN ORDERS RATE`.
`analyze.py` uses only the Python standard library and writes
`results/summary.md`, `results/summary.csv`, `results/analysis.json` and nine SVG
plots (bar charts for p99.9 and throughput, CDFs for queue, allocator,
scheduling, network and the SPSC matrix).

Every configuration in the matrix is expected to produce the checksums in
Section 10; `verify.sh` fails if `epoll` and `io_uring` disagree on trades,
traded quantity or notional.

The sanitizer pass is not decorative: AddressSanitizer caught a real defect while
this report was being prepared. `OrderBookFast` never released the heap nodes of
orders still resting when the book was destroyed, so the `fast-heap` variant
leaked one 64-byte allocation per resting order. The destructor now walks both
sides and releases them; the leak is gone (`verify.sh` is clean) and the
measurements above are unaffected, because the destructor runs after the
measurement window.

---

## 10. Correctness and OS Concepts

### 10.1 Correctness and parity

- 7/7 unit and integration tests pass in Release and under ASan/UBSan; the
  ring-buffer tests pass under TSan; `epoll` and `io_uring` produce identical
  results in `verify.sh`.
- Identically seeded matrix runs produce **one** checksum group for latency
  (788 580 trades, quantity 20 127 113, notional 201 270 962 137) and **one** for
  throughput (1 560 056 trades, quantity 39 843 705, notional 398 436 546 776)
  across all queue, allocator, scheduling and network variants.
- The component benches reproduce the same result across all four queue/book
  combinations (789 497 trades, traded quantity 20 135 763).

Correctness is therefore not conditional on any performance number.

### 10.2 OS concepts covered

| Concept | Where it appears in this project |
|---|---|
| Threads and scheduling | Network thread, engine thread, client; CFS placement; run-queue wait from `schedstat` |
| Mutual exclusion, condition variables | Mutex ring buffer; 157k–199k voluntary switches measured, and the 50 k/s median cost of them |
| Atomics, memory ordering, false sharing | SPSC `release`/`acquire`, `alignas(64)` head/tail, cached peer indices; padded-vs-packed measurement |
| CPU affinity | `pthread_setaffinity_np`; pinning engine, network thread and client, and what each did |
| Real-time scheduling | `SCHED_FIFO` attempt, denied by `rtprio_limit=0`; documented null result |
| Context switching | Voluntary vs involuntary switches, and their correlation with the queue type |
| Memory management | Slab free list vs `operator new`; rehash-induced stalls in `std::unordered_map` |
| I/O subsystems, syscalls | `epoll` drain loop vs `io_uring` single-shot recv; syscall counts invert between load regimes |
| Measurement | `CLOCK_MONOTONIC`, per-order samples, percentiles, warmup, medians, clean-run gating, permutation tests |

---

## 11. Conclusion

The project set out to change one OS mechanism at a time and explain the latency
distribution. It did, and the explanation is less flattering to the
optimizations than expected:

- The matching engine is **~94 ns** of work in a pipeline whose p99.9 is
  **~87 µs**; 51–74 % of that tail is TCP ingress and thread scheduling, and
  86–88 % of the median is ingress and wake-up.
- The lock-free queue earns its place, but in the tail, in throughput and in
  overload behaviour — not in p50. It absorbs 6.40 M orders/s that the mutex path
  can lose to a 33 ms median, at the cost of a permanently busy core.
- The slab allocator's value is worst-case insurance; a fixed-size heap
  allocation is equally fast in the common case, and the dramatic 6.5 ms tail
  belongs to the naive book, not to `malloc`.
- Affinity and `io_uring` did not measurably help on this host. Pinning without
  isolation moved the mechanism (run-queue wait 2.20 → 0.22 ms) without a
  significant end-to-end effect, and pinning the producer's thread actively hurt.
  The mechanisms that explain this — SMT siblings, run-queue delay, wakeups,
  drain loops — are the actual subject of the course.
- The host is a variable of the same order as the code. A controlled confinement
  experiment moves p99.9 from 2.4 ms to 142 µs with no code change, which is why
  every claim here is attached to a counter and a clean-run count.

The most useful artifact is not the fastest configuration; it is the ability to
say, for any given run, *which stage* consumed the tail, with a counter that
proves it.

---

## 12. Future Work

- Re-run the matrix under `isolcpus` + `performance` governor + turbo off, with
  `SCHED_FIFO`, on a dedicated host, to measure the realtime arm that this host
  forbade.
- Repeat the 6.4 M/s mutex sweep with enough repetitions to estimate the collapse
  probability, and find the offered rate at which it becomes deterministic.
- Add PMU counters (`perf stat`) for cache and branch behaviour once
  `perf_event_paranoid` allows it.
- Multi-instrument partitioning (one engine per symbol) rather than a
  multi-threaded book.
- Replace loopback TCP with a NIC and measure the effect of interrupt affinity and
  busy-polling on the arrival component.
