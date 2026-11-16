## Concurrent Low-Latency Order Matching Engine

### 2. Team Execution Model — read this before assigning work

The numbered iterations below (§5–§12) describe a **template**: baseline version → optimized version → measure → explain, applied once per subsystem. They are **not** a single queue the whole team works through one at a time — that would turn an 8-week parallel project into a 10-stage serial one and blow the timeline.

Instead, map iterations to owners and run them **concurrently** against the frozen core:

| Owner | Runs this template, in parallel with everyone else |
|---|---|
| A | Iteration 1 only — owns the frozen core everyone else builds against. Hard gate: must land by end of week 2. |
| B | Iterations 2 → 3 (mutex ring buffer → lock-free SPSC), on their own branch |
| C | Iteration 4 (new/delete → slab allocator), on their own branch |
| D | Iterations 5 → 6 (default scheduling → CPU affinity → RT scheduling), on their own branch |
| E | Iterations 7A → 7B (epoll → io_uring) **and** owns the shared benchmarking harness everyone else's iterations depend on |

Only **Iteration 8 (integration matrix)** and **Iteration 9 (profiling)** are serial by necessity — they require every owner's optimized version to already exist. Everything before that is parallel.

---

### 3. Fixed System Architecture

The core architecture remains stable throughout:

```text
Clients / Producers
        |
        v
 Network Ingestion
 (epoll / io_uring)
        |
        v
   Ring Buffer
(mutex / lock-free)
        |
        v
 Matching Engine
   single-threaded
        |
        v
    Order Book
        |
        v
     Trades
```

The matching engine remains **single-threaded**. Concurrency exists at the boundaries. Introducing a multi-threaded order book would turn this into a substantially different, much harder research problem — deliberately out of scope.

---

### 4. Iteration 0 — Project Infrastructure

**Objective:** common repo, build system, testing framework, benchmark structure — before any optimization.

**Work:** C++17/20 CMake project; common headers/interfaces; Git workflow; unit tests; ASan/UBSan; TSan (for later concurrency work); common benchmark output format.

**Success criteria:** `git clone && cmake && make` produces the same working project for everyone. No performance optimization at this stage.

---

### 5. Iteration 1 — Correct Single-Threaded Matching Engine (Owner: A)

**Objective:** simplest correct engine, before concurrency or low-level optimization.

**Implement:** `Order`, `Trade`, `OrderBook`, price-time priority, buy/sell matching, resting orders, partial fills, multiple price levels. Use `std::map` + `std::deque` as specified — do not optimize the data structure yet.

**Test cases (must pass before anyone depends on this):**

1. Buy order on empty book.
2. Exact buy/sell match.
3. Partial fill.
4. Matching across multiple price levels.
5. FIFO ordering at the same price.
6. Unmatched order resting in the book.

**Measurement:** matching latency (p50/p99/p999) *without* network or concurrency overhead.

**Deliverable:** correct, single-threaded engine with a reproducible isolated baseline. **Hard gate — must merge to `main` by end of week 2.**

---

### 6. Iteration 2 — Mutex-Protected Ring Buffer (Owner: B, part 1)

Fixed-size circular buffer, `std::mutex`, correctness-first. This is the concurrent baseline.

**Measure:** throughput, p50/p99/p999, CPU utilization, queue-full events, queue-empty events.

---

### 7. Iteration 3 — Lock-Free SPSC Queue (Owner: B, part 2)

Replace mutex + shared state with atomic head/tail (producer owns tail, consumer owns head; acquire/release ordering).

**Correctness before performance — non-negotiable order of operations:**

1. Sustained producer/consumer stress test.
2. Run under ThreadSanitizer.
3. Verify no orders lost, no orders duplicated, ordering preserved, full/empty behavior correct.
4. Only *then* benchmark.

**Experiment:** Iteration 2 vs. Iteration 3, everything else held constant (same workload, CPU, scheduler, allocator, network layer, matching logic, benchmark).

**Main question:** does removing the mutex actually reduce latency, and at what contention level does the difference become significant? Do not assume lock-free always wins.

---

### 8. Iteration 4 — Memory Allocation (Owner: C)

Baseline: `new`/`delete`. Optimized: slab/pool allocator with an in-place free list.

**Measure (both configs):** allocation latency, deallocation latency, matching latency, p50/p99/p999, variance, throughput.

**Main question:** does avoiding general-purpose allocation reduce latency, or — more importantly — reduce latency *variability*? Look specifically for whether allocator behavior shows up in the tail.

---

### 9. Iteration 5 — CPU Affinity (Owner: D, part 1)

Baseline: default Linux scheduler (thread may migrate cores). Optimized: pinned to a dedicated core via `sched_setaffinity`.

**Measure:** p50/p99/p999, throughput, CPU migrations, scheduling-related variation.

**Main question:** does keeping the matching engine on a consistent CPU reduce latency jitter? Focus on **tail** latency, not the average.

---

### 10. Iteration 6 — Real-Time Scheduling (Owner: D, part 2)

Compare: default (`SCHED_OTHER`) vs. pinned vs. pinned + `SCHED_FIFO`.

**Measure:** p50/p99/p999, maximum observed latency, throughput, scheduling behavior.

**Main question:** does RT scheduling reduce tail latency, and what trade-offs does it introduce? Treat this as an experiment — a null result is still a valid result.

**Note:** confirm `CAP_SYS_NICE`/root access on the actual test machine in week 1, not week 7.

---

### 11. Iteration 7A — epoll Baseline (Owner: E, part 1)

Traditional readiness-based path: `epoll_wait` → `read()` → parse → push to ring buffer.

**Measure:** network-to-engine latency, throughput, p50/p99/p999, CPU utilization, syscall counts.

---

### 12. Iteration 7B — io_uring (Owner: E, part 2)

Replace epoll+read with submission/completion via shared rings.

**Experiment:** epoll vs. io_uring under identical client count, order rate, order distribution, message size, CPU configuration, matching workload.

**Main question:** under what workload, if any, does io_uring provide measurable benefit over epoll? Isolate syscall count/CPU time specifically, not just wall-clock — that's the actual mechanism being compared.

---

### 13. Iteration 8 — Integrated Optimization Matrix (all owners — serial by necessity)

Only runnable once B, C, D, and E each have both their baseline and optimized versions ready.

| Dimension | Config A | Config B |
|---|---|---|
| Queue | Mutex | Lock-free SPSC |
| Scheduling | Default | CPU pinned / RT |
| Network | epoll | io_uring |

8 configuration combinations total, run through the same shared harness so results are comparable. Keep a spreadsheet of tested combos — it's easy to think you tested something and hadn't.

---

### 14. Iteration 9 — Profiling and Explanation

Not another optimization — the analysis phase. For every significant difference, use `perf stat`/`perf record`/`perf report` to find the *mechanism*: less blocking, fewer context switches, less lock contention, different cache behavior, fewer migrations, fewer syscalls. An unexplained chart is worth far less than two correct sentences of reasoning.

---

### 15. Iteration 10 — Final Stable System

Choose the final configuration based on measurements, not on which one sounds more advanced, uses newer technology, or resembles a production exchange.

---

### 17. Standard Measurement Protocol

For **every** configuration tested, across every iteration:

1. **Build** the system for that exact configuration.
2. **Warm-up:** run the workload and discard the **first 10,000 orders** before recording anything (JIT/cache warm-up, allocator pool fill, TLB warm-up).
3. **Workload:** generate a controlled, fixed order rate and distribution — same generator seed across configs being compared.
4. **Timestamp** every order at creation (`submit_time`), record latency to trade generation.
5. **Repeat the full run 10 times** per configuration. Report the median across runs for each percentile, and flag any run whose p999 deviates from the median by more than ~20% for investigation before including it.
6. **Environment controls, set once and logged every run:**
   - CPU governor set to `performance` (not `powersave`/`ondemand`) — inconsistent clocking corrupts exactly the tail latency you're measuring.
   - Disable CPU frequency boost (turbo) for reproducibility, or explicitly test both and report which.
   - Isolate the benchmark's dedicated core(s) from the rest of the system where possible (`isolcpus`, or at minimum close background processes).
   - Record: CPU model, core count, kernel version, governor setting, whether SMT/hyperthreading was enabled, `liburing` version.
7. **Collect:** throughput, p50, p99, p999, max latency.
8. **Investigate** any anomalous result before writing it into the report — don't paste a surprising number without checking it wasn't noise (thermal throttling, a background process, a run that didn't warm up).
9. **Store raw per-order samples**, not just the computed percentiles — you'll want to re-slice the data during the analysis phase (Iteration 9).

The harness (owned by E) is shared across every configuration so all comparisons use the same measurement mechanism.

---

### 18. The Optimization Ladder (per-component template — run in parallel, see §2)

```text
                    CORRECTNESS
                         │
                         ▼
              ┌────────────────────┐
              │ 1. Single-threaded │  ← A, hard gate, week 2
              │    Matching Engine │
              └─────────┬──────────┘
                        │
     ┌──────────────────┼──────────────────┬──────────────────┐
     ▼                  ▼                  ▼                  ▼
┌─────────┐       ┌───────────┐      ┌───────────┐      ┌───────────┐
│ B: Queue│       │ C: Alloc  │      │ D: Sched  │      │ E: Network│
│ mutex → │       │ new/del → │      │ default → │      │ epoll →   │
│lock-free│       │ slab      │      │ pinned →RT│      │ io_uring  │
└────┬────┘       └─────┬─────┘      └─────┬─────┘      └─────┬─────┘
     └──────────────────┴──────────────────┴──────────────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 8. Integrated      │  ← serial, needs everyone's output
              │    Benchmarking    │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 9. perf Analysis   │
              │    & Explanation   │
              └────────────────────┘
```

---

### 19. Explicitly Out of Scope (initially)

Multi-threaded order-book access, MPSC/MPMC lock-free queues, NUMA-awareness, kernel bypass, DPDK, RDMA, custom network/transport protocols, FPGA acceleration, SIMD matching, exotic lock-free structures, production-grade exchange features, distributed matching. Interesting, but outside the 8-week learning objective.

---

### 20. Final Project Narrative

> We started with a correct but straightforward matching engine, built in parallel with the rest of the team's tooling and testing setup. We established its isolated performance baseline, then re-validated it once integrated. Each subsystem owner independently ran the same baseline→optimize→measure→explain template for their component — synchronization, memory allocation, CPU scheduling, and network I/O — using a shared benchmarking harness so every comparison was apples-to-apples. We then combined the components into a controlled 8-way configuration matrix and used `perf` to explain every significant difference in terms of the actual OS mechanism responsible, rather than just reporting the numbers.

The central question remains:

> **What happens to latency and tail latency when we systematically change the underlying OS mechanisms, and can we explain the results?**

## Concurrent Low-Latency Order Matching Engine

# 2. Fixed System Architecture

The core architecture will remain stable throughout the project:

```text
Clients / Producers
        |
        v
 Network Ingestion
 (epoll / io_uring)
        |
        v
   Ring Buffer
(mutex / lock-free)
        |
        v
 Matching Engine
   single-threaded
        |
        v
    Order Book
        |
        v
     Trades
```

The matching engine itself will remain **single-threaded**.

Concurrency will primarily exist at the system boundaries, particularly between order producers and the matching engine. This keeps the project tractable while still allowing meaningful experiments with synchronization and scheduling.

This architectural constraint is deliberate: introducing a multi-threaded order book would turn the project into a substantially different and much harder research problem.

---

# 3. Iteration 0 — Project Infrastructure

## Objective

Establish a common repository, build system, testing framework, and benchmarking structure before implementing optimizations.

## Work

- Create the C++17/20 CMake project.
- Establish common headers and interfaces.
- Establish Git workflow.
- Set up unit tests.
- Set up AddressSanitizer/UndefinedBehaviorSanitizer.
- Set up ThreadSanitizer for later concurrency experiments.
- Establish a common benchmark output format.

## Success Criteria

The entire team should be able to:

```bash
git clone ...
cmake ...
make
```

and obtain the same working project.

No performance optimization is performed at this stage.

---

# 4. Iteration 1 — Correct Single-Threaded Matching Engine

## Objective

Build the simplest correct matching engine before introducing concurrency or low-level optimization.

## Implementation

Implement:

- `Order`
- `Trade`
- `OrderBook`
- Price-time priority
- Buy-side matching
- Sell-side matching
- Resting orders
- Partial fills
- Multiple price levels

The order book will initially use:

```text
std::map
    +
std::deque
```

as specified in the project design.

## Testing

The following correctness cases must pass:

1. Buy order on empty book.
2. Exact buy/sell match.
3. Partial fill.
4. Matching across multiple price levels.
5. FIFO ordering at the same price.
6. Unmatched order resting in the book.

## Measurement

At this stage, measure the matching engine **without network and concurrency overhead**.

Record:

- Orders processed/sec
- Matching latency
- p50
- p99
- p999

## Deliverable

A completely correct, single-threaded matching engine with a reproducible baseline benchmark.

### Important Rule

**Do not optimize the order-book data structures yet.**

The purpose of this iteration is to establish the correctness and performance baseline.

---

# 5. Iteration 2 — Introduce Concurrent Order Ingestion

## Objective

Introduce producer/consumer concurrency while keeping the matching engine itself unchanged.

```text
Producer Thread(s)
       |
       v
 Ring Buffer
       |
       v
Matching Engine Thread
```

The order book should not need to know that orders are being produced concurrently.

---

## 2A. Mutex-Protected Ring Buffer

First implement:

```text
Producer
   |
 mutex
   |
Ring Buffer
   |
 mutex
   |
Consumer
```

Use:

- fixed-size circular buffer
- `std::mutex`
- simple correctness-first synchronization

This is explicitly the planned baseline for the queue.

### Measure

Under identical workloads, measure:

- Throughput
- p50 latency
- p99 latency
- p999 latency
- CPU utilization
- queue-full events
- queue-empty events

This becomes the **concurrent baseline**.

---

# 6. Iteration 3 — Replace Mutex With Lock-Free SPSC Queue

## Objective

Remove blocking synchronization from the producer/consumer boundary.

Replace:

```text
mutex + shared state
```

with:

```text
atomic head
atomic tail
```

using an SPSC lock-free ring buffer.

The producer owns the tail index and the consumer owns the head index, with acquire/release memory ordering used to establish visibility between them.

---

## Correctness Before Performance

Before benchmarking:

- Run sustained producer/consumer stress tests.
- Run ThreadSanitizer.
- Verify no orders are lost.
- Verify no orders are duplicated.
- Verify ordering.
- Verify behavior when the queue becomes full/empty.

Only after correctness is established should performance be measured.

## Experiment

Compare:

```text
Iteration 2:
Mutex Ring Buffer

             VS

Iteration 3:
Lock-Free SPSC Ring Buffer
```

Everything else must remain unchanged.

## Main Question

> Does removing the mutex actually reduce latency, and under what workload does the difference become significant?

---

# 7. Iteration 4 — Memory Allocation Optimization

## Objective

Investigate the effect of dynamic memory allocation in the matching engine's hot path.

The baseline system uses normal dynamic allocation.

Create:

```text
Baseline:
new / delete

        VS

Optimized:
Slab / Pool Allocator
```

The slab allocator pre-allocates storage for `Order` objects and maintains a free list, avoiding repeated general-purpose allocation/deallocation in the hot path.

---

## Experiment

Run the same workload through:

### Configuration A

```text
Standard allocator
new/delete
```

### Configuration B

```text
Custom slab allocator
```

Measure:

- Allocation latency
- Deallocation latency
- Matching latency
- p50
- p99
- p999
- Variance
- Throughput

## Main Question

> Does avoiding general-purpose allocation reduce latency or, more importantly, latency variability?

The experiment should specifically investigate whether allocator behavior becomes visible in the tail of the latency distribution.

---

# 8. Iteration 5 — CPU Affinity

## Objective

Investigate the effect of CPU scheduling and thread migration on latency.

The matching engine thread will first run under the normal Linux scheduler.

Then it will be pinned to a dedicated CPU core using CPU affinity.

```text
Configuration A

Linux scheduler
      |
Matching Engine
      |
CPU may vary


Configuration B

CPU affinity
      |
Matching Engine
      |
Dedicated CPU
```

CPU affinity using `sched_setaffinity` / pthread affinity is part of the planned scheduling experiment.

---

## Experiment

Compare:

```text
Default scheduling
        VS
CPU-pinned matching thread
```

Measure:

- p50
- p99
- p999
- Throughput
- CPU migrations
- Scheduling-related variation

## Main Question

> Does keeping the matching engine on a consistent CPU reduce latency jitter?

The primary focus should be **tail latency**, not merely average latency.

---

# 9. Iteration 6 — Real-Time Scheduling Experiment

## Objective

Investigate whether Linux real-time scheduling policies affect matching-engine latency.

Compare:

```text
A. SCHED_OTHER
B. CPU affinity + SCHED_FIFO
```

The proposal specifically identifies `SCHED_OTHER`, `SCHED_FIFO`, and `SCHED_RR` as scheduling configurations to investigate.

The initial experiment can focus on:

```text
Default
    VS
Pinned
    VS
Pinned + SCHED_FIFO
```

as defined in the team plan.

---

## Measure

- p50
- p99
- p999
- Maximum observed latency
- Throughput
- Scheduling behavior

## Main Question

> Does real-time scheduling reduce tail latency, and what trade-offs does it introduce?

This should be treated as an **experiment**, not automatically as an optimization.

If the measurements do not improve, that is still a valid result.

---

# 10. Iteration 7 — Network Ingestion

## Objective

Move from an internal order generator to actual network-based order ingestion.

The matching engine interface remains unchanged.

Only the ingestion layer changes.

```text
Client
  |
  v
Network
  |
  +----> epoll
  |
  +----> io_uring
  |
  v
Ring Buffer
  |
  v
Matching Engine
```

The project intentionally compares `epoll` and `io_uring` as two implementations of the same network-ingestion interface.

---

# 11. Iteration 7A — epoll Baseline

## Objective

Implement the traditional Linux event-driven network path.

```text
Socket
   |
epoll_wait()
   |
read()
   |
parse order
   |
ring buffer
```

This becomes the network baseline.

Measure:

- Network-to-engine latency
- Throughput
- p50
- p99
- p999
- CPU utilization
- System calls

---

# 12. Iteration 7B — io_uring

## Objective

Replace the `epoll + read` path with `io_uring`.

```text
Socket
   |
io_uring
   |
Completion
   |
parse order
   |
ring buffer
```

The experiment should investigate whether asynchronous submission/completion and batching reduce system-call overhead and improve latency under load.

---

## Experiment

Compare:

```text
epoll
  VS
io_uring
```

under identical:

- Number of clients
- Order rate
- Order distribution
- Message size
- CPU configuration
- Matching workload

## Main Question

> Under what workload, if any, does io_uring provide measurable benefits over epoll?

Again, the result does not need to favor io_uring.

---

# 13. Iteration 8 — Integrated Optimization Matrix

At this point, individual components have been independently tested.

Now combine them.

The major configuration dimensions are:

| Dimension  | Configuration A | Configuration B |
| ---------- | --------------- | --------------- |
| Queue      | Mutex           | Lock-free SPSC  |
| Scheduling | Default         | CPU pinned / RT |
| Network    | epoll           | io_uring        |

The benchmarking harness should allow these configurations to be changed without modifying the matching logic.

The planned system therefore produces controlled combinations such as:

```text
Mutex + Default + epoll
Lock-free + Default + epoll
Mutex + Pinned + epoll
Lock-free + Pinned + epoll
...
```

The project plan identifies the complete comparison as **8 configuration combinations** for the main queue/scheduling/network dimensions.

---

# 14. Iteration 9 — Profiling and Explanation

This is not another optimization.

This is the **analysis phase**.

For every significant performance difference, investigate:

> Why did this happen?

Use tools such as:

```text
perf stat
perf record
perf report
```

and relevant Linux/system measurements.

For example:

```text
Lock-free is faster
        ↓
Why?
        ↓
Less blocking?
Less context switching?
Less lock contention?
Different cache behavior?
```

Similarly:

```text
CPU affinity improves p999
        ↓
Why?
        ↓
Fewer migrations?
Less scheduling interference?
```

And:

```text
io_uring does not improve latency
        ↓
Why?
        ↓
Workload too small?
Network not the bottleneck?
Extra implementation overhead?
```

The final report should contain **explanations backed by measurements**, not simply benchmark charts. This matches the project's definition of success.

---

# 15. Iteration 10 — Final Stable System

Only after all experiments are complete should the team choose the configuration used for the final demonstration.

The final configuration should be selected based on the project's measurements and requirements.

It should **not** be selected because:

- it sounds more advanced,
- it uses newer technology,
- it has more lock-free code,
- or it resembles a production exchange.

The final system should simply represent the experimentally justified configuration produced by the project.

---

# 17. Standard Measurement Protocol

Every major iteration should use the same measurement methodology.

For each configuration:

1. Build the system.
2. Run a warm-up period.
3. Generate a controlled workload.
4. Timestamp orders consistently.
5. Record latency for every order.
6. Collect:

   - throughput
   - p50
   - p99
   - p999
   - maximum latency where useful
7. Repeat the experiment multiple times.
8. Record system configuration.
9. Investigate anomalous results.
10. Store raw results.

The benchmark harness is intentionally shared across configurations so that every before/after comparison is based on the same measurement mechanism.

---

# 18. The Optimization Ladder

The complete project can therefore be viewed as this ladder:

```text
                    CORRECTNESS
                         │
                         ▼
              ┌────────────────────┐
              │ 1. Single-threaded │
              │    Matching Engine │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 2. Mutex Queue     │
              │    + Concurrency   │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 3. Lock-Free SPSC  │
              │    Queue            │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 4. Slab Allocator  │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 5. CPU Affinity    │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 6. RT Scheduling   │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 7. epoll           │
              │    Network Layer   │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 8. io_uring        │
              │    Comparison      │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 9. Integrated      │
              │    Benchmarking    │
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │ 10. perf Analysis  │
              │     & Explanation  │
              └────────────────────┘
```

---

# 20. Final Project Narrative

The final project should tell one coherent engineering story:

> **We started with a correct but straightforward matching engine. We established its performance baseline. We then introduced concurrency using a mutex-protected queue and measured the resulting behavior. We replaced the mutex with an SPSC lock-free queue and experimentally evaluated the difference. We then investigated memory allocation, CPU affinity, real-time scheduling, and network I/O using the same methodology. Finally, we combined the components and profiled the resulting system to understand where the latency actually came from.**

This gives the project a clear progression:

```text
Correctness
    ↓
Baseline
    ↓
Concurrency
    ↓
Synchronization optimization
    ↓
Memory optimization
    ↓
Scheduling optimization
    ↓
I/O optimization
    ↓
Integration
    ↓
Profiling
    ↓
Evidence-based conclusions
```
