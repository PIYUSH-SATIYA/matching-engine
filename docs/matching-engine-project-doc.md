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
| **Raw results** | `results/summary.md`, `results/components/`, `results/env.txt` |

> This document is the authoritative version. The bundled
> `matching-engine-project-doc.pdf` was generated from the **proposal** and does
> not contain the measurements below.

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

Three results carry the report:

1. **The engine is not the bottleneck, and the tail is not in the matcher.**
   Measured in isolation, the optimized book matches or rests an order in
   **109 ns (p50)** and **456 ns (p99.9)**; producer→queue→engine with no network
   is **370 ns (p50)** and **5.0 µs (p99.9)**. The full TCP pipeline is
   **24.5 µs (p50)** and **159.8 µs (p99.9)** on a clean run, of which the
   arrival stage (client send → network thread parse) accounts for **79–89 %** of
   the p99.9. The engine-side remainder is ~20 µs, still four times the 5 µs
   standalone pipeline floor, so even that share is mostly queue wait and polling
   phase rather than matching.

2. **The queue choice is invisible at p50 and dominant in the engine-side tail.**
   SPSC vs mutex, holding everything else fixed: p50 24.5 µs vs 21.8 µs (within
   run-to-run noise), but the *service* component at p99.9 is **24.5 µs vs
   119 µs**, throughput **4.82 M/s vs 2.89 M/s**, and the network thread's
   voluntary context switches are **7 vs 168 442**. The mechanism is the
   condition-variable wakeup path: a blocked consumer must be woken and scheduled
   for every batch, while an SPSC consumer that is already spinning pays nothing.

3. **Affinity without isolation did not help, and one control made it worse.**
   Pinning the engine to one CPU (no `isolcpus`, no realtime priority available)
   left the blocking queue paying wakeup latency on a shared core: mutex/pinned
   service p99.9 rose from 119 µs to **367 µs** and cumulative engine run-queue
   wait from 0.4 ms to **37.7 ms**. Pinning the *network* thread to a single CPU
   raised its p99.9 from 158 µs to **831 µs**. `SCHED_FIFO` was denied
   (`rtprio_limit=0`, no `CAP_SYS_NICE`), so the realtime arm of the experiment is
   a documented null result.

A separate controlled check (Section 6.10) establishes that the multi-millisecond
outliers that appear in some rows are host contention delaying ingress, not a
property of the pipeline: confining an unrelated desktop process to the E-cores
moved the same configuration from **2.40 ms to 142 µs** p99.9 while the engine's
own CPU time was unchanged.

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
  probe, because it turned out to be harmful (Section 6.9).
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
(p50/p90/p99/p99.9/max), the arrival/service split, per-thread CPU time, context
switches, minor/major faults, network syscall counts, SPSC yields, ingest
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

**Per configuration (latency).**

1. Release build (`-O2 -DNDEBUG`), rebuilt between phases.
2. Client sends 1 010 000 orders at a fixed 200 000 orders/s, seed 42
   (open loop: the client paces itself, so a slow server cannot slow the sender).
3. Discard the first 10 000 orders as warmup.
4. Timestamp at client send; stop the clock when the engine completes the order.
5. Five repetitions (ten for the two configs that needed re-measurement);
   report the median; keep every raw run.
6. Flag runs whose p99.9 exceeds 1.2× the configuration median, and list (not
   delete) runs where ingress was delayed.

**Per configuration (throughput).** Same client, rate 0 (unthrottled),
2 000 000 orders, 3 repetitions. This is what the queue and allocator cost when
no one is waiting.

**Component floors (no network).** `bench_order_book` runs 2 000 000 mixed
operations (70 % new orders, 30 % cancels) against a single-threaded book.
`bench_pipeline` runs producer → queue → engine at the same 200 000 orders/s with
producer on CPU 6 and engine on CPU 2. These isolate the parts that the TCP
pipeline hides.

**Environment (recorded in `results/env.txt`).** Intel i5-12450HX: four P-cores
with SMT (CPUs 0–7) and four E-cores (CPUs 8–11). Governor `powersave`, turbo on,
SMT on, `isolcpus` **not** used, uid 1000, `rtprio_limit=0`,
`perf_event_paranoid=2`, `perf`/`strace`/`cpupower` not installed. The host is a
desktop session, and `env.txt` records its load and top CPU consumers at capture
time.

**What could not be done, and what we did instead.**

| Planned control | Status | Consequence |
|---|---|---|
| `performance` governor, turbo off | no permission | CPU frequency is not fixed; p50 values carry frequency variance |
| `isolcpus` for the engine core | no permission | "pinned" ≠ "isolated"; this becomes a *finding*, Section 6.6 |
| `SCHED_FIFO` | denied (`EPERM`) | realtime arm is a documented null result |
| `perf stat` / `perf record` | unavailable | mechanism evidence comes from in-process counters (`schedstat`, `getrusage`, `NetStats`) instead of PMU events |
| quiet machine | not available | host contention is measured per run and reported, not hidden (Section 6.10) |

---

## 6. Results

All latency numbers are **client send → engine complete** on the same host, in
microseconds unless stated. "Clean" means the run had no order that was already
older than 1 ms when the server parsed it — i.e. the host did not delay ingress.
Values are medians across repetitions.

### 6.1 Component floors (no network)

`bench_order_book`, 2 000 000 ops, single thread, engine on CPU 2:

| book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| V1 `map`+`list`+`unordered_map` | 9.56 M/s | 99 ns | 487 ns | 701 ns | 14.0 ms |
| V3 array+slab+bitmap | 29.11 M/s | 109 ns | 276 ns | 456 ns | 21.7 µs |

V3 is **3× the throughput** with a *higher* p50 (109 ns vs 99 ns) and a p99.9
similar in magnitude (456 ns vs 701 ns). Its contribution is not typical-case speed;
it is that V3's worst case is **21.7 µs** while V1's is **14.0 ms** — a factor of
645. A median-only report would have missed the entire point of the data
structure.

`bench_pipeline`, 1 010 000 orders at 200 000 orders/s, producer CPU 6, engine
CPU 2:

| queue | book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| mutex | V1 | 2.74 M/s | 1827 ns | 2797 ns | 7.12 ms | 11.98 ms |
| mutex | V3 | 3.02 M/s | 1824 ns | 2294 ns | 21.4 µs | 1.08 ms |
| spsc | V1 | 4.97 M/s | 372 ns | 1001 ns | 8.31 ms | 13.04 ms |
| spsc | V3 | 6.05 M/s | 370 ns | 603 ns | 5.05 µs | 35.2 µs |

This is the cleanest view of both OS mechanisms in the project, because no TCP is
involved. SPSC removes the wakeup: p50 drops from 1.82 µs to 0.37 µs (4.9×) and
p99.9 from 21.4 µs to 5.05 µs (4.2×) for the same book. V3 removes the allocator
stall: with either queue, V1's p99.9 is 7–8 ms and V3's is 5–21 µs. All four
configurations produced byte-identical trade counts and quantities
(789 497 trades, 20 135 763 traded quantity), so the speed did not come from
matching less.

### 6.2 Integrated latency matrix

200 000 orders/s, 1 010 000 orders, 10 000 warmup. n = 5 unless noted.

| config | n | clean | p50 | p99 | p99.9 | p99.9 (clean) |
|---|---|---|---|---|---|---|
| mutex / default / epoll | 5 | 4 | 21.8 µs | 106.4 µs | 182.9 µs | 178.8 µs |
| mutex / default / uring | 5 | 5 | 27.3 µs | 113.2 µs | 177.8 µs | 177.8 µs |
| mutex / pin / epoll | 5 | 5 | 30.5 µs | 88.3 µs | 450.6 µs | 450.6 µs |
| mutex / pin / uring | 5 | 5 | 31.2 µs | 116.0 µs | 374.9 µs | 374.9 µs |
| spsc / default / epoll | 5 | 5 | 24.5 µs | 46.7 µs | 159.8 µs | 159.8 µs |
| spsc / default / uring | 5 | 5 | 26.9 µs | 102.3 µs | 162.2 µs | 162.2 µs |
| spsc / pin / epoll | 10 | 0 | 24.8 µs | 953.7 µs | 2.55 ms | – |
| spsc / pin / uring | 10 | 4 | 25.5 µs | 1.31 ms | 2.71 ms | 135.4 µs |
| allocator: fast-slab | 5 | 5 | 25.4 µs | 71.5 µs | 158.2 µs | 158.2 µs |
| allocator: fast-heap | 5 | 5 | 23.8 µs | 76.1 µs | 160.9 µs | 160.9 µs |
| allocator: v1 book | 5 | 5 | 22.5 µs | 92.2 µs | 9.60 ms | 9.60 ms |

Reading the table:

- Every configuration that could be measured cleanly sits at **p50 22–31 µs and
  p99.9 135–180 µs**, except the V1 book (9.6 ms) and the mutex/pinned rows
  (375–451 µs). The spread across queue, network and scheduler choices is small
  compared with the distance from those numbers to the component floors.
- `spsc / pin / epoll` has **zero clean runs out of ten**. It is reported, not
  removed, but it is not evidence about the pipeline: the host delayed ingress in
  every attempt (Section 6.10 explains why and gives the controlled measurement).

### 6.3 Where the tail lives: arrival vs service

Same runs, medians. *Arrival* = client send → network-thread parse. *Service* =
parse → engine completion (queue wait + matching). "Share" is arrival p99.9 as a
fraction of total p99.9, using clean runs.

| config | p50 total | arrival p50 | service p50 | p99.9 total | arrival p99.9 | service p99.9 | arrival share |
|---|---|---|---|---|---|---|---|
| spsc / default / epoll | 24.5 µs | 20.6 µs | 4.6 µs | 159.8 µs | 125.9 µs | 24.5 µs | 79 % |
| spsc / default / uring | 26.9 µs | 23.3 µs | 4.0 µs | 162.2 µs | 139.2 µs | 24.1 µs | 86 % |
| spsc / pin / uring (clean) | 24.4 µs | 21.1 µs | 4.6 µs | 135.4 µs | 115.6 µs | 18.5 µs | 85 % |
| mutex / default / epoll (clean) | 23.0 µs | 11.1 µs | 7.0 µs | 178.8 µs | 151.7 µs | 119.1 µs | 85 % |
| mutex / default / uring | 27.3 µs | 21.2 µs | 5.9 µs | 177.8 µs | 133.7 µs | 110.8 µs | 75 % |
| mutex / pin / epoll | 30.5 µs | 15.4 µs | 11.8 µs | 450.6 µs | 138.8 µs | 367.1 µs | 31 % |
| allocator: fast-slab | 25.4 µs | 21.1 µs | 4.6 µs | 158.2 µs | 141.6 µs | 22.1 µs | 89 % |
| allocator: v1 book | 22.5 µs | 18.4 µs | 6.4 µs | 9.60 ms | 141.5 µs | 9.58 ms | 1 % |

This table is the core result. For every configuration built on the optimized
book, arrival accounts for **75–89 %** of p99.9, and the engine-side service
component is 18–25 µs with SPSC. The V1 book inverts the ratio: arrival is still
~140 µs, but service is 9.58 ms, so 99 % of the tail is the book.

The service p99.9 of ~20 µs is itself interesting: the standalone pipeline
achieves 5 µs. The difference is the polling loop's phase (an order pushed just
after the consumer last checked is picked up on the next pass) plus sharing a
machine with the network thread. This is a real cost of a busy-spin consumer and
is reported rather than smoothed away.

### 6.4 Queue: mutex vs lock-free SPSC

Fixed: fast-slab book, epoll, default scheduling, 200 000 orders/s.

| metric | mutex | SPSC | ratio |
|---|---|---|---|
| p50 | 21.8 µs | 24.5 µs | 0.9× (noise) |
| p99.9 | 182.9 µs | 159.8 µs | 1.1× |
| **service p99.9** | **119.1 µs** | **24.5 µs** | **4.9×** |
| throughput (unthrottled) | 2.89 M/s | 4.82 M/s | 1.7× |
| engine CPU per order | 0.35 µs | 0.21 µs | 1.7× |
| network thread voluntary switches | 168 442 | 7 | 24 000× |
| engine run-queue wait | 0.38 ms | 3.50 ms | – |

**Mechanism.** At p50 the two queues are the same because at 200 000 orders/s the
queue is almost never empty; both hand off in nanoseconds. The difference appears
where the consumer has to wait. The mutex queue parks the consumer on a condition
variable, so each batch costs a wakeup: the producer must enter the kernel, pick a
CPU for the consumer, and the consumer resumes with a cold pipeline. That cost
shows up as 168 442 voluntary switches and 119 µs of *service* tail, not as
arrival tail. The SPSC consumer never sleeps: it spins, sees `tail_` change
(usually within a few hundred nanoseconds), and proceeds; the counter for
"yielded because the queue looked empty" is in the tens per run at this rate.

The throughput rows confirm the same mechanism from the other direction: 2.89 vs
4.82 M/s, and 0.35 vs 0.21 µs of engine CPU per order. The SPSC buffer does its
own waiting instead of asking the kernel to do it.

Caveat: p50 for mutex is *lower* here. Across the four default-scheduling rows the
p50 values span 21.8–27.3 µs with run-to-run variation of a few microseconds, so
we treat p50 as a tie and base the conclusion on the service tail and throughput,
which differ by 5× and 1.7× consistently.

### 6.5 Allocator: slab vs heap vs the V1 book

Fixed: SPSC, epoll, pinned, 200 000 orders/s.

| book / allocator | p50 | p99.9 | service p99.9 | max | throughput |
|---|---|---|---|---|---|
| V3 + slab | 25.4 µs | 158.2 µs | 22.1 µs | 250 µs | 4.82 M/s |
| V3 + `operator new` | 23.8 µs | 160.9 µs | 31.9 µs | 326 µs | 4.40 M/s |
| V1 map/list/malloc | 22.5 µs | 9.60 ms | 9.58 ms | 14.17 ms | 3.75 M/s |

The honest result is that **slab and heap are indistinguishable in the integrated
harness** (158 vs 161 µs p99.9, overlapping ranges). A fixed 64-byte
`operator new`/`delete` pair is served by glibc's per-thread cache in tens of
nanoseconds, and 140 µs of ingress noise hides the difference. Slab's benefit is
in the worst case, which the component benchmark exposes: V3's max is 21.7 µs,
and the heap variant's integrated max is 326 µs versus the slab's 250 µs.

The V1 row is the real allocator/data-structure lesson. Its service p99.9 is
**9.58 ms**, arrival is normal (141 µs), and the run-to-run p99.9 range is
9.53–9.89 ms — a reproducible stall, not a random outlier. Looking at the raw
per-order samples, the slow orders arrive in a handful of clusters of arbitrary
size (431, 1046, 2738 orders) that begin at arbitrary points in the run, whereas
host-delay clusters are built from whole 64-order client batches. That is the
signature of a *book-side* stall, not an ingress stall.

Mechanism: `std::unordered_map` grows by rehashing, and each rehash reallocates and
relinks every bucket; `std::map` and `std::list` allocate one tree/list node per
operation from the general heap. The maximum (14.0 ms) matches the standalone V1
book benchmark's maximum (14.0 ms) almost exactly, which confirms the attribution
without a profiler. The V3+heap control rules out `operator new` alone as the
cause: same allocator call, 3 orders of magnitude smaller tail, because the sizes
are fixed and the containers never rehash.

### 6.6 Scheduling: affinity without isolation

Fixed: SPSC and mutex, fast-slab, epoll. `default` = nothing pinned; `pin` =
engine on CPU 2.

| config | p50 | p99.9 | service p99.9 | engine run-queue wait | involuntary switches |
|---|---|---|---|---|---|
| spsc / default | 24.5 µs | 159.8 µs | 24.5 µs | 3.5 ms | 175 |
| spsc / pin | 24.8 µs | (not measurable cleanly) | 4.2 µs | 0.5 ms | 48 |
| mutex / default | 21.8 µs | 182.9 µs | 119.1 µs | 0.4 ms | 153 |
| mutex / pin | 30.5 µs | 450.6 µs | 367.1 µs | 37.7 ms | 131 |

**Mechanism.** Pinning the engine to CPU 2 does not give it CPU 2. Without
`isolcpus` (and without realtime priority, which the host denies), ordinary
desktop work is still scheduled on that core, and a pinned thread cannot migrate
away from it.

The two queue types respond differently, and the difference is diagnostic:

- With the **blocking** (mutex) queue, the engine is asleep between batches. Every
  wakeup is a *new* scheduling decision, and it can land behind whatever else is
  queued on CPU 2. Cumulative run-queue wait jumps from 0.4 ms to 37.7 ms across a
  5-second run, and service p99.9 rises from 119 µs to 367 µs.
- With the **spinning** (SPSC) queue, there is no wakeup to delay: the engine is
  already runnable. Its own run-queue wait stays at 0.5 ms and its service p99.9
  is unaffected. Whatever tail appears in that row is at ingress, which is why the
  row cannot be measured cleanly on this host.

So pinning did not reduce any percentile. It converted a working default into one
that is at the mercy of a core it does not own. The expected benefit requires
`isolcpus`/cgroup cpuset isolation, which we could not apply.

`SCHED_FIFO` at priority 50 was attempted and denied (`EPERM`); `rtprio_limit=0`
and the binary has no capabilities. `results/rt-attempt.txt` records the failure
and the error text. This arm is a null result by host policy, not by design.

### 6.7 Network: epoll vs io_uring

Fixed: SPSC, fast-slab, pinned. Latency at 200 000 orders/s; kernel operations
counted per run.

| config | p50 | p99.9 (clean) | kernel ops (paced, 1.01 M orders) | kernel ops (unthrottled, 2 M orders) | throughput |
|---|---|---|---|---|---|
| epoll | 24.5–24.8 µs | 159.8 µs | 47 331 | 20 178 | 4.82 M/s |
| io_uring | 25.5 µs | 135.4 µs | 31 566 | 40 328 | 4.76 M/s |

Latency and throughput are indistinguishable. The mechanism difference is in
system calls, and it **inverts** between the two load regimes:

- **Paced (producer slower than consumer).** The client sends 64-order batches
  every 320 µs. io_uring needs one recv SQE and one completion per batch
  (≈ 15.8 k × 2 ≈ 31.6 k ops). epoll needs one `epoll_wait` *plus* at least one
  `recv` that returns data and one that returns `EAGAIN` to detect the drain
  (≈ 15.8 k waits + 31.6 k reads ≈ 47.3 k ops). io_uring wins by 33 %.
- **Unthrottled (consumer can never catch up).** The socket buffer is always
  non-empty, so epoll's drain loop services many `recv`s per readiness event and
  waits only when it finally catches up (≈ 20.2 k ops for 2 M orders). io_uring's
  single-shot recv re-posts one SQE per read (≈ 19.6 k reads → ≈ 40.3 k ops).
  epoll wins by 50 %.

That is the answer to "when does io_uring help": it removes the per-event wait
when traffic is bursty and the reader is faster than the writer; it does not help
— and costs more submissions — when the reader is saturated, because it has no
drain loop to amortize over. Neither difference moved a latency percentile here,
because the dominant cost at 200 000 orders/s is not syscall count.

### 6.8 Throughput

Unthrottled client, 2 000 000 orders, 3 repetitions each.

| config | orders/s | engine CPU/order | net CPU/order | net nvcsw | SPSC yields |
|---|---|---|---|---|---|
| spsc / pin / uring | 4.86 M/s | 0.21 µs | 0.20 µs | 11 | 17 |
| spsc / default / epoll | 4.82 M/s | 0.21 µs | 0.20 µs | 7 | 9 |
| spsc / default / uring | 4.76 M/s | 0.21 µs | 0.21 µs | 14 | 30 |
| spsc / pin / epoll | 4.59 M/s | 0.22 µs | 0.21 µs | 14 | 25 |
| allocator: v1 book | 3.75 M/s | 0.27 µs | 0.26 µs | 9 | 224 |
| mutex / default / epoll | 2.89 M/s | 0.35 µs | 0.24 µs | 168 442 | 0 |
| mutex / pin / uring | 3.00 M/s | 0.33 µs | 0.23 µs | 156 944 | 0 |

SPSC saturates at ≈ 4.8 M orders/s with 0.21 µs of engine CPU per order; the
mutex queue at ≈ 3.0 M/s and 0.33 µs, and pays 150 000+ voluntary switches for it.
The network thread costs the same 0.20–0.26 µs per order in every configuration,
which is the control that says the queue is what changed.

### 6.9 Probes: attributing the residual tail

All probes use SPSC, fast-slab, pinned engine, 200 000 orders/s, `ME_INGEST_STATS=1`.

| probe | p50 | p99.9 | orders > 1 ms before parse | max arrival |
|---|---|---|---|---|
| baseline (`probe-ingest-epoll`) | 19.5 µs | 157.6 µs | 0 / 1 000 000 | 215 µs |
| client **not** pinned (`probe-client-free`) | 24.3 µs | 153.0 µs | 0 / 1 000 000 | 206 µs |
| `io_uring` baseline | 24.5 µs | 173.7 µs | 0 / 1 000 000 | 278 µs |
| network thread pinned to CPU 4 | 24.5 µs | **831.1 µs** | **704 / 1 000 000** | 2.57 ms |

Two conclusions:

- **Client pinning is not the source of the tail** (153.0 vs 157.6 µs with the
  client free), so the pinning of the client is a legitimate control, not a way of
  hiding client jitter.
- **Pinning the network thread hurts, badly.** Confining it to a single core
  removes its ability to migrate away from unrelated work: 0.07 % of orders
  arrive more than a millisecond late and the max arrival delay is 2.57 ms. This
  is the same lesson as Section 6.6, measured on the producer side.

### 6.10 Host sensitivity: proving the outliers are not the pipeline

The matrix ran on a desktop session, not a dedicated test box. One process (a
terminal emulator, pid 43546) consumed ≈ 380 % CPU continuously for its entire
lifetime; the load average was 5–7 and reached 26 during one block. Some rows
therefore contain millisecond-scale outliers.

We did not paper over this. Two lines of evidence establish that the outliers are
host contention delaying *ingress*:

1. **Per-run counters.** Every contaminated run shows the same fingerprint:
   `net_ingest_slow` in the thousands, `net_runq_ns` in the tens or hundreds of
   milliseconds, arrival p99.9 ≈ 2.5 ms, and *service* p99.9 unchanged at ~100 µs.
   Clean runs have `net_ingest_slow = 0`. `results/summary.md` lists every
   contaminated run individually.

2. **A controlled A/B.** The same binary and configuration was run twice: once
   with the desktop process free to use all 12 CPUs, once with it confined to the
   E-cores for the duration and then restored (no process killed, reniced or
   stopped; affinity restored by a shell `trap`).

   | condition | p50 | p99 | p99.9 | max | network run-queue wait | orders > 1 ms |
   |---|---|---|---|---|---|---|
   | desktop free | 26.0 µs | 1046 µs | 2400 µs | 2896 µs | 97.7 ms | 10 017 |
   | desktop confined to E-cores | 22.5 µs | 68 µs | 142 µs | 240 µs | 1.2 ms | 0 |

   The engine's own run-queue wait in the confined half was 0.29 ms, so the engine
   was not the delayed component. Full transcript in
   `results/host-isolation.txt`.

Because of this, the report quotes medians over all repetitions but marks how many
runs were clean, and treats a configuration with zero clean runs as
"not measurable on this host" rather than as evidence about the code.

---

## 7. Answering the Central Question

**1. Where does latency go?** On loopback TCP, it goes to ingress, not to
matching. The book needs 109 ns (p50) and 456 ns (p99.9); the queue adds roughly
260 ns; the network path adds ~20 µs at p50 and ~130 µs at p99.9. Any optimisation
aimed at the matcher would have been invisible.

**2. Does synchronization matter?** Yes, but at the point where a thread has to
wait, and not at p50. Replacing a condvar queue with an SPSC ring cuts the
engine-side p99.9 by 4.9× and raises throughput 1.7×, because it replaces a
kernel wakeup with a spin. The 168 442 → 7 voluntary switches are the mechanism,
not a correlate.

**3. Does the allocator matter?** Only in the worst case, and only when the
allocation pattern is irregular. A fixed-size `new` is as good as a slab in this
workload. The 9.6 ms V1 tail comes from growing, pointer-chasing containers
(rehash + per-node allocation), and the V3+heap control isolates the cause.

**4. Does affinity help?** Not without isolation. Pinning did not improve any
percentile and made the blocking-queue tail 3× worse, because a pinned thread on a
shared core still waits behind other work and can no longer escape. Realtime
priority would have changed the picture, but it was unavailable.

**5. Is `io_uring` faster?** Not in latency or throughput here. Its benefit is
syscall count under bursty, reader-faster-than-writer traffic (47.3 k → 31.6 k
ops), and it reverses under saturation (20.2 k → 40.3 k). "Fewer syscalls" is not
the same as "lower latency".

---

## 8. Limitations and Threats to Validity

- **Host noise.** The measurements were taken on a loaded desktop. The clean-run
  filter and the A/B check make the attribution defensible, but absolute p50
  values (22–31 µs) carry frequency and contention variance that a dedicated host
  would not have.
- **CPU frequency is not fixed** (`powersave` governor, turbo on), so run-to-run
  comparisons at the microsecond level are weaker than the millisecond-level
  effects the report relies on.
- **No PMU counters.** `perf` was unavailable, so cache misses, branch misses and
  IPC are absent. Mechanism claims rest on in-process counters
  (`schedstat`, `getrusage`, syscall counts) plus the A/B controls, not on
  hardware events.
- **Loopback only.** Client and server share the host, so there is no NIC, no
  interrupt affinity and no kernel bypass. The arrival component would look
  different on real hardware, and probably larger.
- **One queue depth (16 384) and one offered rate (200 k/s).** Backpressure and
  queue-full behaviour are not exercised; the throughput runs deliberately avoid
  them.
- **The arrival component includes client-side work.** The load generator stamps a
  batch once, after its pacing wait but before it generates the 64 messages and
  calls `send`. Arrival therefore contains a few microseconds of client message
  generation plus the syscall, and it is quantised to batch boundaries (which is
  also why host-delay clusters appear as whole 64-order batches). This affects the
  arrival number, not the service number, and it cannot change the direction of the
  arrival/service attribution.
- **Six to ten repetitions per configuration**, not hundreds. Medians are
  reported with the full p99.9 range so the reader can see the spread.
- **The 5-member table contains a duplicated roll number** (`202451130` for both
  Piyush Satiya and Prayag Kirar). It is reproduced as submitted; we did not
  invent a correction.
- **`spsc / pin / epoll` has no clean measurement** on this host, so the queue
  comparison relies on the default-scheduling rows, the component benchmark, and
  the A/B check.

---

## 9. Reproducing the Results

```bash
# build and test
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j"$(nproc)"
ctest --test-dir build-rel --output-on-failure     # 7/7

# full verification: Release + ASan/UBSan + TSan + epoll/io_uring parity
./scripts/verify.sh

# record the environment, then run the whole chain
./scripts/env.sh
ME_LAT_RUNS=5 ME_THR_RUNS=3 ./scripts/run_matrix.sh --force
ME_LAT_RUNS=5 ME_THR_RUNS=3 ./scripts/run_experiments.sh
ME_COMPONENT_REPS=3 ./scripts/run_components.sh
ME_PROBE_REPS=3 ./scripts/run_probe.sh
python3 ./scripts/analyze.py
```

`analyze.py` writes `results/summary.md`, `results/summary.csv`,
`results/analysis.json` and seven SVG plots (bar charts for p99.9 and throughput,
CDFs for queue, allocator, scheduling, network and the SPSC matrix). It uses only
the Python standard library. A single measurement can be repeated with
`scripts/run_one.sh CONFIG QUEUE BOOK NET SCHED KIND RUN ORDERS RATE`.

Every configuration in the matrix is expected to produce the checksums in
Section 6.1; `verify.sh` fails if `epoll` and `io_uring` disagree on trades,
traded quantity or notional.

The sanitizer pass is not decorative: AddressSanitizer caught a real defect while
this report was being prepared. `OrderBookFast` never released the heap nodes of
orders still resting when the book was destroyed, so the `fast-heap` variant leaked
one 64-byte allocation per resting order. The destructor now walks both sides and
releases them; the leak is gone (`verify.sh` is clean) and the measurements above
are unaffected, because the destructor runs after the measurement window.

---

## 10. OS Concepts Covered

| Concept | Where it appears in this project |
|---|---|
| Threads and scheduling | Network thread, engine thread, client; CFS placement; run-queue wait from `schedstat` |
| Mutual exclusion, condition variables | Mutex ring buffer; 168 k voluntary switches measured |
| Atomics, memory ordering, false sharing | SPSC `release`/`acquire`, `alignas(64)` head/tail, locally cached indices |
| CPU affinity | `pthread_setaffinity_np`; pinning engine, network thread and client, and what each did |
| Real-time scheduling | `SCHED_FIFO` attempt, denied by `rtprio_limit=0`; documented null result |
| Context switching | Voluntary vs involuntary switches, and their correlation with the queue type |
| Memory management | Slab free list vs `operator new`; rehash-induced stalls in `std::unordered_map` |
| I/O subsystems, syscalls | `epoll` drain loop vs `io_uring` single-shot recv; syscall counts invert between load regimes |
| Measurement | `CLOCK_MONOTONIC`, per-order samples, percentiles, warmup, medians, outlier flagging |

---

## 11. Conclusion

The project set out to change one OS mechanism at a time and explain the latency
distribution. It did, and the explanation turned out to be less flattering to the
optimisations than expected:

- The matching engine is **~100 ns** of work in a pipeline whose p99.9 is
  **~160 µs**; 79–89 % of that tail is TCP ingress and thread scheduling.
- The lock-free queue earns its place, but in the engine-side tail and in
  throughput, not in p50.
- The slab allocator's value is worst-case insurance; a fixed-size heap allocation
  is equally fast in the common case, and the dramatic 9.6 ms tail belongs to the
  naive book, not to `malloc`.
- Affinity and `io_uring` did not help on this host, and pinned-to-one-core
  controls made things worse. The mechanisms that explain this — SMT siblings,
  run-queue delay, wakeups, drain loops — are the actual subject of the course.

The most useful artifact is not the fastest configuration; it is the ability to
say, for any given run, *which stage* consumed the tail, with a counter that
proves it.

---

## 12. Future Work

- Re-run the matrix under `isolcpus` + `performance` governor + turbo off, with
  `SCHED_FIFO`, on a dedicated host, to measure the realtime arm that this host
  forbade.
- Add PMU counters (`perf stat`) for cache and branch behaviour once
  `perf_event_paranoid` allows it.
- Queue-depth and offered-rate sweep to find where the SPSC buffer starts to
  apply backpressure.
- Multi-instrument partitioning (one engine per symbol) rather than a
  multi-threaded book.
- Replace loopback TCP with a NIC and measure the effect of interrupt affinity and
  busy-polling on the arrival component.
