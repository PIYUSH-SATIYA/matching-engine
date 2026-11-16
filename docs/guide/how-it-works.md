# How this project works

## What this document is

This is the "read this second" document. The
[project report](../matching-engine-project-doc.md) is written for a grader and
states results compactly; this one walks the same system from the beginning, at a
slower pace, for a reader who knows what a program and a benchmark are but has not
built a lock-free concurrent service in C++ before.

It is organised the way the work actually depends on itself:

1. the system shape and why it is that shape,
2. the design decisions, which are the part worth arguing about,
3. each component on its own,
4. how the components are wired into one running program,
5. what is measured, and why it is measured that way,
6. how to read each result and what it does and does not prove.

Every number quoted here appears in `results/summary.md`,
`results/components-summary.md`, or Section 6 of the report. Nothing is rounded
in a way that changes a conclusion.

**If you have ten minutes,** read §2 (the design decisions) and §6.5–6.7 (the
integrated latency table, the arrival/service split, and the queue comparison).
Those three are the argument of the project; the rest is evidence and detail.

---

## 1. The problem, in plain terms

An exchange receives orders. A **limit order** says "buy 10 units at no more than
price 101" or "sell 4 units at no less than 100". Orders that cannot trade
immediately wait in a list. That list is the **order book**. When a new order
arrives that can trade against waiting orders, the engine produces **trades** and
removes (or shrinks) the orders it matched.

Two rules define correct behaviour:

* **Price priority** — the best price trades first (highest bid, lowest ask).
* **Time priority** — at the same price, the order that arrived first trades
  first.

So the system has to do three things: get orders in (network), decide what happens
to each order (matching), and keep a correct book (data structures). The fourth
thing — knowing *how long* each step took — is what turns this from a program into
an experiment.

### The quantity that matters is the tail, not the average

Over the 1 000 000 measured orders, the median round trip is about 25 µs, and the
part from "order parsed" to "match finished" is about 3 µs. The median is almost
useless on its own — it says nothing about the orders that went wrong.
What a trading system cares about is the **worst** orders: if 999 000 orders are
served in 25 µs and 1 000 take 30 ms, the average still looks fine and the system
is unusable. This is why every table in
this project reports percentiles:

| symbol | meaning |
|---|---|
| p50 | median — half the orders were faster than this |
| p99 | 1 order in 100 was slower than this |
| p99.9 | 1 order in 1 000 was slower than this |
| max | the single slowest order in the run |

The interesting engineering is almost entirely in the gap between p50 and p99.9.

### The shape of the machine

Two threads and one queue:

```
client process                      server process
┌───────────────┐            ┌──────────────────────────────────────────┐
│ load_client   │  40-byte   │ network thread          engine thread    │
│ paces orders  │──TCP──────▶│ recv → parse → push ──▶ pop → match →    │
│ stamps send   │            │            [ queue ]      update book     │
└───────────────┘            └──────────────────────────────────────────┘
```

One thread does I/O, one thread does matching, and they meet at a queue. The
client is a separate process on the same machine, so "latency" means: timestamp
written by the client before `send` → timestamp read by the server after the
engine finished the order. That path includes the loopback network stack, which
is deliberate: it is the path a real order takes.

Why two threads at all? Because a single thread that both waits on sockets and
matches orders wastes matching time inside `epoll_wait`, and a single thread that
only matches cannot receive while it matches. Splitting the two means the matching
thread never blocks on the network. The price is a hand-off, and the hand-off is
where the tail lives.

---

## 2. Design decisions

These are ordered roughly from "shapes everything else" to "local". Each one
lists what it buys and what it costs, because a decision without a cost is
usually a decision that was not understood.

### D1 — The book is owned by exactly one thread

Only the engine thread ever reads or writes the order book. There is no lock
around the book, not even an uncontended one.

**Buys:** no synchronization on the hot path at all; every data-structure
optimisation below is legal because nobody else can observe an intermediate
state.

**Costs:** the design cannot scale matching across cores. If the engine thread
cannot keep up, the only options are a faster book or sharding by instrument. That
trade is accepted, and the measurements justify it: the book alone sustains
33.68 M ops/s, while the full pipeline tops out at 7.35 M orders/s, so the matching
work was never the limiting factor here.

**Where you see it:** all `book` work in the results is single-threaded, and the
engine's measured cost per order (0.17 µs) is per core, not divided by anything.

### D2 — One queue, one producer, one consumer (SPSC)

There is exactly one network thread and exactly one engine thread, so the queue
needs one producer and one consumer. It does not need general multi-producer
support, and it does not get it.

**Buys:** the producer owns the write index and the consumer owns the read index.
Nothing is ever written by both threads, so no compare-and-swap loop is needed;
one store with release ordering is enough.

**Costs:** the design is not reusable for N producers. Adding a second network
thread would require a different queue.

**Where you see it:** the queue section (§6.4 of the report) is the only place
where concurrency is compared, and the comparison is exactly "blocking MPMC-safe
mutex queue" vs "non-blocking SPSC queue".

### D3 — Keep the slow implementation, and make it the definition of correct

The project keeps a deliberately naive order book (`std::map` + `std::list` +
`std::unordered_map`) and a deliberately naive queue (mutex + condition
variables). Both are used in the benchmark matrix.

**Buys:** correctness has an independent definition. The fast book must produce
byte-identical results to the slow one on the same order stream; the fast queue
must do the same. Optimisation can then never quietly change semantics — the
checksum comparisons in §10.1 of the report exist for this.

**Costs:** roughly twice the code, and two implementations to keep in sync.

**Where you see it:** `bench_order_book` runs both books on the same 2 000 000
operations; the pipeline benchmark runs all four queue × book combinations; every
integrated run is checked against one expected checksum.

### D4 — The engine is a template over the book and the event sink

`MatchingEngine<Book, Sink>` takes the book type and the event consumer as
compile-time parameters.

**Buys:** matching logic exists once. The compiler generates a specialised version
per combination, so there is no virtual call, no pointer-to-function, and no
branch on "which book am I using" inside the loop. Swapping books is a
configuration change, not a code change, which is what makes the comparison fair.

**Costs:** every combination is compiled separately (larger binary, slower build),
and the interfaces must match exactly.

**Where you see it:** `src/main.cpp` contains a small dispatcher
(`runWithBook<Queue>`) that instantiates the combinations from command-line flags.

### D5 — Price levels are array indices, not tree nodes

The fast book stores one slot per price: `levels_[price - min_price]`.

**Buys:** finding a price level becomes pointer arithmetic instead of walking a
red-black tree; no allocation, no pointer chasing, no branch misprediction storm.

**Costs:** prices must fall inside a fixed window. Outside it, the order is
rejected by `validPrice()`. This is a real limitation, stated in the report, not
hidden.

**Where you see it:** V3 versus V1 in the book benchmark — 33.68 vs 11.63 M
ops/s — and the 10.97 ms max of V1, which is the tree's rebalancing/allocation
tail.

### D6 — One resting order = one cache line

Each resting order is a `Node` with `alignas(64)`: the 48-byte `Order` plus two
pointers is exactly 64 bytes, enforced by `static_assert(sizeof(Node) == 64)`.

**Buys:** touching an order brings in exactly the bytes that order needs, and
nothing else. A neighbour's data can never be dragged into the cache by a
different order.

**Costs:** 64 bytes per resting order instead of 56 packed; with the book's
capacity that is memory you pay for whether or not it is used. Also, 64 is the
cache-line size of this CPU family, not a universal constant.

**Where you see it:** the difference between V3's 26.7 µs worst case and V1's
10.97 ms; and the false-sharing experiment, which isolates the same idea at the
queue boundary.

### D7 — A slab allocator with the free list inside the free slots

`SlabAllocator<T, N>` grabs one block up front, cuts it into `sizeof(T)` chunks,
and threads the free list through the unused chunks themselves.

**Buys:** allocate is "pop a pointer", deallocate is "push a pointer": two memory
operations, no metadata, no system call, constant time, and the free-list node is
free because it lives in memory that is already unused.

**Costs:** capacity is fixed at construction; a full slab must return failure
(the book turns that into a rejected order). It also cannot give memory back to
the OS.

**Where you see it:** allocator microbenchmark 12.59 vs 17.14 ns per allocation
(1.36×) and 14.13 vs 22.19 ns per free (1.57×); and, more importantly, V3's tail
staying at microseconds while V1's reaches milliseconds.

### D8 — The id index is a pre-sized open-addressing table with backward-shift deletion

`OrderId → Node*` is a flat array probed linearly, sized at 2× capacity so it never
grows or rehashes.

**Buys:** one memory access in the common case, no buckets, no node allocations,
no rehash pause. Deletion shifts later entries back instead of leaving tombstones,
so the table cannot degrade into long probe runs after churn.

**Costs:** deletion is O(cluster length), not O(1); the capacity is fixed; the
hash function is assumed good enough (ids are sequential in this workload).

**Where you see it:** cancels are 30 % of the book benchmark; a tombstone-based
table would slow down over a long run, which is a tail effect rather than a
throughput effect.

### D9 — Best price comes from a bitmap, not from the container

A two-level bitmap records which price levels are non-empty; the best price is
found with `ctz`/`clz` (count trailing/leading zeros).

**Buys:** "what is the best bid?" is a handful of instructions, independent of how
many levels exist. `std::map::begin()` is a tree walk with pointer chasing.

**Costs:** every empty↔non-empty transition must update the bitmap, so `add`,
`remove` and `cancel` each carry a little extra work. And the bitmap has a fixed
size, consistent with D5.

**Where you see it:** V3's p50 is *not* better than V1's (94 ns vs 81 ns) —
the bookkeeping is why — while the tail is dramatically better.

### D10 — Put the measurement boundary on the queue, and timestamp twice

Every `Order` carries two timestamps:

* `submit_time` — written by the client before `send`;
* `enqueue_time` — written by the network thread after parsing.

So `submit → enqueue` is the **arrival** component (client, TCP, network thread
scheduling), and `enqueue → completion` is the **service** component (queue +
matching). The report calls the split "arrival versus service".

**Buys:** the question "where did this order spend its time?" becomes a
measurement instead of an argument. Without the split, a 2.4 ms tail could be
blamed on the engine when it actually happened before the engine saw the order.

**Costs:** 8 bytes per order and one clock read on the ingress path; also the
client and server must share a clock, which restricts the setup to one host.

**Where you see it:** the decomposition table, and every conclusion about the
tail. This is the single most useful decision in the project.

### D11 — Instrument the server, not the analysis script

Counters live where the events happen: queue full/empty waits, network accepts /
`recv` calls / `epoll_wait` calls / `io_uring` submissions, orders older than 1 ms
at parse time, per-thread `getrusage`, run-queue wait from `/proc/thread-self/schedstat`,
CPU migrations, faults.

**Buys:** one CSV row per run contains the explanation along with the latency
numbers, so a surprising tail can be attributed after the fact without re-running
anything.

**Costs:** 46 columns of output, and instrumentation that must be careful not to
change what it measures (migration sampling is opt-in for exactly that reason).

**Where you see it:** the throughput table's `net nvcsw` column (12–15 vs
157 000–199 000) is the mechanism behind the queue result; the clean-run gate
(§5.3 of the report) is built from `net_ingest_slow`.

### D12 — The baseline queue blocks on condition variables on purpose

The mutex queue parks the consumer when the queue is empty and the producer when
it is full.

**Buys:** it makes the kernel do the scheduling. Every empty→non-empty transition
costs a wakeup, and every full→not-full transition costs a context switch. That is
precisely the cost the lock-free queue is being compared against — a spinning
mutex queue would have hidden it.

**Costs:** in an unthrottled 2 000 000-order run the producer pays 157 000–199 000
voluntary context switches, and throughput is roughly 3.3 M/s instead of 5.9 M/s.
At the 200 000 orders/s measurement rate the consumer parks 18 500–33 200 times per
run.

**Where you see it:** the report's queue section would be meaningless without this
choice. It is written down in `docs/design/ring-buffer-decision.md`.

### D13 — The SPSC queue's rules: one writer per index, cached peer index, 64-byte separation, bounded spin

Concretely:

* indices are monotonic counters masked with `& (Capacity-1)`, so the queue never
  wastes a slot and "full" is one subtraction;
* each side caches the other side's index and only re-reads the shared atomic when
  the queue *looks* full or empty — this removes most cross-core cache traffic;
* the data write happens before `tail_.store(release)`, and the consumer does
  `tail_.load(acquire)` before reading the slot. That is the minimum ordering that
  is correct; anything stronger costs cycles for nothing;
* `head_` and `tail_` are on separate 64-byte lines, so the two threads never
  write to the same line;
* waiting is bounded: spin with `pause`, and yield only once per 4096 spins. Yielding
  every iteration turns into a syscall storm that costs more than the spin.

**Buys:** a hand-off that costs nanoseconds instead of a kernel round trip.

**Costs:** the consumer spins, so the engine thread burns a full core even when
idle (101 % CPU in the latency runs, against 8 % for the mutex version). That is a
real operational cost and it is reported as one.

**Where you see it:** the pipeline floor of 299 ns p50, the 12 voluntary switches
per run in throughput mode, and the saturation sweep where SPSC absorbs 6.40 M/s
with zero full waits.

### D14 — Network backends sit behind a function-pointer seam

Both servers expose `INetworkServer` and push parsed orders through the same
`PushFn`. The queue and engine cannot tell epoll from io_uring.

**Buys:** epoll vs io_uring becomes a controlled experiment: same parse code, same
push path, same matching. The only difference is the syscall shape (drain-until-
`EAGAIN` with `epoll_wait`, versus one `recv` SQE re-posted per completion).

**Costs:** one extra indirection per order, and the seam forces both backends into
one `run()` signature.

**Where you see it:** epoll performs 47 346 kernel operations per run, io_uring
31 569; the p99.9 result is 85.6 µs vs 96.4 µs. Fewer syscalls did not buy a
better tail.

### D15 — Treat the host as a variable, and say so when a run is contaminated

The runs were taken on a desktop machine, not a dedicated test box. Instead of
pretending otherwise, the harness records the environment, and each run is
classified as *clean* or not: a run is clean only if no order was already more than
1 ms old when the network thread parsed it.

**Buys:** contaminated runs are identified by a mechanism, not by deleting
inconvenient data. 120 of 122 latency runs were clean; the two exceptions are named
in the report with their counters. The clean-run view answers "what does the pipeline
do when the host cooperates", and the full view answers "what does a user actually see".

**Costs:** two views instead of one headline number, and some configurations cannot be
compared in the clean view if they have no clean runs.

**Where you see it:** the host-sensitivity table, where the same binary moves p99.9
from 2.40 ms to 142 µs with no code change.

### D16 — Statistics that compare runs, not orders

Each configuration is repeated 10 times (5 for throughput), and the unit of
statistics is a **run**, not an order. Significance is computed with an exact
permutation test: take the 20 run-level values, enumerate every way of splitting
them into two groups of 10, and count how often the observed difference is
matched or exceeded. With C(20,10) = 184 756 splits, the test has no resampling
error and its resolution floor is about 1×10⁻⁵.

**Buys:** p-values that mean what they say, and an honest answer of "not
distinguishable" when the data cannot separate two configurations
(e.g. queue p50, p = 0.145).

**Costs:** the test can only speak about the runs that exist. With n = 10 it cannot
resolve small differences, which is why several results are reported as "same
within noise" rather than as wins.

**Where you see it:** the statistics and queue sections (§5.4 and §6.4 of the report).

---

## 3. The components, one at a time

Each component is presented as: what it is, how it works, and what it contributes
to the measurements. File paths are relative to the repository root.

### 3.1 `include/order.hpp` — the data that moves

Four aliases (`OrderId`, `Price`, `Quantity`, `Timestamp`) and three structs:

* `Order` — id, side, price, quantity, `submit_time`, `enqueue_time`.
* `Trade` — trade id, both order ids, price, quantity, execution time.
* `EngineEvent` — a small notification (`OrderAccepted`, `TradeExecuted`, …).

This file is the contract between every other file. If you change `Order`, you
change the wire format, the book node size, the queue slot size, and the CSV, all
at once. That is why it is small and why it is first.

Design content: the two timestamps (D10), and the fact that `Order` is 48 bytes so
that `Node` lands on exactly one cache line (D6).

### 3.2 `include/wire.hpp` — how bytes on a socket become an `Order`

A packed 40-byte message: `u64 id`, `u8 side`, 7 bytes padding, `i64 price`,
`u64 quantity`, `u64 client_ts_ns`. `static_assert` pins the size. `valid()` does
cheap rejection. `parse()` walks a receive buffer, emits only *complete* messages,
and moves the partial tail to the front of the buffer.

Why it looks like this: TCP is a byte stream, not a message stream. A `recv` can
return half a message or three and a half messages. The parser is the piece that
turns that into orders, and keeping it separate from sockets means it can be unit
tested without a network at all (`tests/test_wire.cpp` feeds it split, coalesced,
truncated and garbage buffers).

### 3.3 `include/matching_engine.hpp` — the rules

`MatchingEngine<Book, Sink>::process(const Order&)`:

1. read the clock **once**;
2. reject zero quantity, out-of-window price, duplicate id;
3. emit `OrderAccepted`;
4. while the incoming order crosses the best opposite level:
   * trade at the **resting** order's price,
   * emit `TradeExecuted`,
   * subtract quantity,
   * call `book_.reduce()`;
5. rest the remainder, emitting `OrderPartiallyFilled` first if any of it traded;
   if the book's order pool is exhausted, reject the remainder instead.

Two details that matter:

* One clock read per order, shared by all events of that order. Reading the clock
  per event would cost more than some of the matching work.
* The resting order's id and price are copied **before** `reduce()`, because
  `reduce()` may free that node. Reading it afterwards is a use-after-free that
  would pass most tests and fail under ASan.

This file is also where the sink interfaces are declared: `NullSink`,
`CountingSink` (trades, quantity, notional — the checksum), `TeeSink` (fan out),
`EventManagerSink` (route to a logger).

### 3.4 `include/order_book.hpp` + `src/order_book.cpp` — V1, the oracle

`std::map<Price, std::list<Order>>` per side, plus
`std::unordered_map<OrderId, iterator>` for cancels.

It is slow on purpose: a tree walk to find the level, a `malloc` per resting
order, a hash lookup with possible rehashing, and `begin()` to find the best
price. It exists so that "correct" has a definition that does not depend on the
optimised code (D3), and so that the optimised code has something honest to be
compared against.

### 3.5 `include/order_book_fast.hpp` — V3, the optimised book

Same public interface (`add`, `reduce`, `cancel`, `validPrice`, `contains`,
`hasBid`/`hasAsk`, `bestBid`/`bestAsk`, `frontAtBestBid`/`frontAtBestAsk`), so the
engine template does not know which book it has. Four things change underneath:

| V1 | V3 | decision |
|---|---|---|
| tree of price levels | `levels_[price - min]` | D5 |
| `std::list` node per order | 64-byte `Node` from a slab | D6, D7 |
| `unordered_map` id index | pre-sized open addressing, backward-shift delete | D8 |
| `map.begin()` for best price | two-level bitmap + `ctz/clz` | D9 |

The template parameter `UseHeap` chooses between `SlabAllocator<Node>` and
`HeapAllocator<Node>`, so the *same* book code can be run on slab memory or on
`operator new` memory. That is what makes the allocator comparison controlled:
only the source of memory changes.

Honest limitations, all enforced in code: prices must be inside a fixed window,
the number of resting orders is capped by the slab, and the id hash assumes
reasonable ids.

### 3.6 `include/slab_allocator.hpp` — memory

`SlabAllocator<T, N>`: one aligned block, `N` chunks, free list threaded through
the free chunks. `allocate()` pops, `deallocate()` pushes. `HeapAllocator<T>`
provides the same interface over `operator new`/`delete`.

### 3.7 `include/event_sinks.hpp` — where events go

Sinks are tiny and templated. `CountingSink` accumulates trades, quantity and
notional so every configuration can be compared by checksum. `NullSink` makes
"measure the book without measurement overhead" possible. `TeeSink` lets a run
both count and log. The engine never knows which one it has.

### 3.8 `include/ring_buffer.hpp` + `src/ring_buffer.cpp` — the baseline queue

Fixed array of 16 384 `Order`s, one mutex, two condition variables, blocking
`push`/`pop`, plus `full_waits`/`empty_waits` counters. `push` unlocks before
`notify_one` so the woken consumer does not immediately block on the mutex.

Its purpose is to be the control (D12). Every property that makes it slow —
kernel wakeups, context switches, the consumer sleeping — is the thing the next
component removes.

### 3.9 `include/spsc_ring_buffer.hpp` — the lock-free queue

The whole implementation is about 40 lines of logic:

```
producer: t = tail_; if (t - cached_head_ >= Capacity) { refresh; if still full -> wait }
          buf_[t & mask] = order; tail_.store(t+1, release)
consumer: h = head_; if (h == cached_tail_) { refresh; if still empty -> wait }
          out = buf_[h & mask]; head_.store(h+1, release)
```

Everything else in the file is the consequences of D2 and D13: cached peer
indices, release/acquire ordering, `alignas(64)` on the two indices, monotonic
counters, bounded spinning, and counters for full/empty waits and yields.

One subtlety worth internalising: the cached index is an *optimisation on the read
path*, not a correctness mechanism. The refresh inside the `if` is what makes the
answer authoritative. Removing the refresh produces a queue that mostly works,
which is the worst kind of bug.

### 3.10 `include/network_layer.hpp`, `src/network_epoll.cpp`, `src/network_io_uring.cpp` — ingress

`network_layer.hpp` defines the seam (`INetworkServer`, `PushFn`, `NetStats`) and
the optional ingress probe `noteIngest()`, which counts orders that were already
older than 1 ms when parsed.

**epoll backend.** One `epoll_wait` over listener + connections, edge-triggered;
each readable event enters `recv()` until `EAGAIN`. Per connection, a 4 KiB buffer
with a `used` cursor; `wire::parse` consumes whole messages and leaves the partial
tail. For each parsed order: set `enqueue_time`, maybe probe ingress age, push.

**io_uring backend.** One accept SQE at start, one single-shot `recv` SQE per
connection re-posted after each completion; `io_uring_wait_cqe_timeout` to wait,
`peek_batch_cqe` to drain. Same parse-and-push body.

The difference is the syscall shape: ~47 k kernel operations for epoll versus
~32 k for io_uring per latency run. That is the experiment.

### 3.11 `include/affinity.hpp` — pinning, and failing honestly

`pinThisThread(cpu)` wraps `pthread_setaffinity_np`. `setRealtimeFifo(priority)`
returns `bool`: if the kernel refuses `SCHED_FIFO` (no `CAP_SYS_NICE`), the app
prints why, records `exit_code=2`, and stops. It does not silently run at normal
priority and label the run "rt".

That choice is why the report can say "`SCHED_FIFO` was refused on this host"
instead of presenting a real-time result that never happened.

### 3.12 `src/main.cpp` — the assembly

Around 550 lines, and the only file that knows all the parts exist. It parses
flags, picks a queue and a book with a small template dispatcher, then:

1. build book → sink → engine → queue → server, with `push` bound to `queue->push`;
2. start the **engine thread**: optional pin, optional RT check, then
   `while (true) { o = queue->pop(); if (o.id == 0) break; engine.process(o); record; }`;
3. start the **network thread**: `server->run(stop)`, then push a sentinel order
   with `id == 0`;
4. join both; read `NetStats`; sort the latency samples; append one CSV row.

Two details:

* The sentinel is the shutdown protocol. A shared boolean would race with the last
  order; an in-band sentinel cannot, because it travels through the same queue.
* Per-thread CPU time, context switches and run-queue wait are measured from a
  baseline taken **after** the first order, so the engine's startup spin is not
  charged to the orders.

The 46-column CSV written here is the raw material for every table in
`results/`.

### 3.13 `tools/load_client.cpp` — the workload

Connects, builds 40-byte messages, and either:

* **paced** (200 000 orders/s): schedule is absolute — `next = start + i / rate` —
  so a slow server cannot slow the sender. Batches of 64. This is an **open loop**:
  offered load is fixed, so latency is measured under a known rate.
* **unthrottled**: batches of 1024 as fast as the socket accepts. This measures
  **capacity**, not latency, because the server's speed now limits the sender.

The seed is fixed, so every configuration sees the same order stream — which is
what makes the checksum comparison meaningful.

### 3.14 `include/benchmark.hpp` and `bench/*.cpp` — the component floors

`LatencyRecorder` keeps a fixed-capacity array of `uint64` nanosecond samples,
`sorted()` for percentiles, and reports overflow instead of silently dropping.

Four microbenchmarks, each changing one thing:

| program | what it isolates |
|---|---|
| `bench_order_book` | book alone: 2 M seeded ops, 70 % add / 30 % cancel, V1 vs V3 |
| `bench_pipeline` | queue + engine, no sockets: the pure hand-off + matching floor |
| `bench_allocator` | alloc/free only, 64-byte nodes, slab vs `operator new` |
| `bench_false_sharing` | two byte-identical SPSC buffers, padded vs packed indices |

"Floor" is the right word: these numbers are the best the component can do, so
they bound how much of an end-to-end result the component can possibly explain.

### 3.15 `scripts/` — how a number is produced

The scripts, from smallest to largest:

* `env.sh` — records host, governor, EPP, frequencies, thermal state, toolchain,
  privileges, load. This is why the report can describe the machine it ran on.
* `run_one.sh` — one server + one client run: builds the arguments, starts the
  server, starts the client pinned, waits, and keeps both logs.
* `run_matrix.sh`, `run_experiments.sh`, `run_saturation.sh`, `run_components.sh`,
  `run_probe.sh` — the five experiments, each a loop over `run_one.sh`.
* `run_session.sh` — the wrapper that makes a session reproducible: clean-tree
  check, power profile, preserve previous results, rebuild, run all phases, run
  both analysis scripts, sample thermals, save a transcript.
* `analyze.py`, `analyze_components.py` — aggregation, clean-run classification,
  flags, plots, and the exact permutation tests.
* `verify.sh` — Release + ASan/UBSan + TSan, plus an end-to-end epoll/io_uring
  parity run.

### 3.16 The build order (three passes)

The code compiles and tests in three passes, and that is also the order to read it
in if you want to reconstruct it:

1. **Deterministic core, no threads:** `order.hpp` → `wire.hpp` →
   `slab_allocator.hpp` → `order_book.hpp` → `order_book_fast.hpp` →
   `matching_engine.hpp` → `event_sinks.hpp`. Gate: V1 and V3 agree
   order-by-order; the wire parser survives hostile buffers.
2. **Concurrency:** `ring_buffer.*` → `spsc_ring_buffer.hpp` → `network_layer.hpp`
   → the two backends → `affinity.hpp` → `main.cpp`. Gate: both queues pass their
   tests under TSan, and the full binary produces the same checksums as pass 1.
3. **Measurement:** `benchmark.hpp` → the four `bench/` programs → the client →
   the scripts. Gate: `verify.sh` green, checksums identical across every
   configuration.

`docs/plan/iteration-plan.md` describes the same decomposition as iterations 1–12.

---

## 4. How it is wired: the life of one order

Take the configuration used for the headline comparison: SPSC queue, fast book on
the slab, epoll, engine pinned, client at 200 000 orders/s. Real numbers from the
median run (p50 path):

| time | what happens | measured as |
|---|---|---|
| t₀ | client stamps `submit_time` and `send`s | start of **total** |
| t₀ + ~21 µs | network thread's `epoll_wait` returns, `recv` delivers 64 messages, `wire::parse` yields 64 orders; `enqueue_time` stamped | end of **arrival** (21.6 µs) |
| t₀ + ~21 µs | `push` writes the order into the ring and advances `tail_`; engine is spinning, sees it within nanoseconds | — |
| t₀ + ~24 µs | engine `pop`s, `process()` reads the clock once, matches, updates the book, emits events to `CountingSink` | end of **service** (3.1 µs) |
| — | `LatencyRecorder` stores `now − submit_time` | **total** 25.0 µs |

Then, after the client disconnects, the network thread pushes the sentinel, the
engine breaks its loop, both threads join, and `main` writes the CSV row: p50,
p99, p99.9, arrival/service split, CPU time, switches, run-queue wait, kernel ops,
queue waits, checksum.

Three things this timeline makes obvious:

1. **Most of the latency is arrival.** At p50, ~21.6 of 25.0 µs is before the
   engine does anything. Improving the book cannot fix that.
2. **The queue is only visible when the engine is waiting.** At 200 000 orders/s
   the engine is faster than the network, so the consumer spins and the queue is
   rarely non-empty for long. That is why p50 is nearly identical for both queues
   and the difference appears in the tail.
3. **Batching shapes the waits.** 1 000 000 measured orders / 64 per batch =
   15 625 batches, and the SPSC queue records 15 796 empty waits — about one per
   batch. The queue is not "slow"; the consumer correctly runs out of work between
   bursts.

### The configuration matrix

Every integrated run is one point in this grid; the server flags are `--queue`,
`--book`, `--net`, `--sched`:

| axis | values | what it changes |
|---|---|---|
| queue | `mutex`, `spsc` | blocking hand-off vs lock-free hand-off |
| book | `v1`, `fast-heap`, `fast-slab` | slow oracle; fast book on `new`; fast book on slab |
| net | `epoll`, `uring` | syscall shape only |
| sched | `default`, `pin`, `rt` | no pinning; engine pinned; engine pinned + `SCHED_FIFO` |

Note what `book` actually controls: `v1` is a *different data structure* (tree +
`malloc`), while `fast-heap` vs `fast-slab` is a clean allocator comparison on
identical book code. That distinction matters when reading the allocator row of
the integrated table — it is not "allocator only".

`rt` never produced a run: `SCHED_FIFO` requires `CAP_SYS_NICE`, which this host
does not grant (`rtprio_limit=0`). The attempt is recorded (`results/rt-attempt.txt`)
rather than quietly dropped.

---

## 5. What is benchmarked, how, and why

There are four kinds of measurement. They answer different questions and they must
not be mixed.

### 5.1 Integrated latency (open loop, 200 000 orders/s)

**How.** 1 010 000 orders, first 10 000 discarded as warm-up, 11 configurations ×
10 repetitions. Client paces against an absolute schedule, so the offered rate is
fixed even if the server is slow.

**Why this shape.** Latency only means something at a stated load. A fixed open
loop measures "how long does an order take when the system is offered 200 000
orders/s", which is a question a user can ask. An unthrottled client would instead
measure "how fast can the system go", where the queue is always full and the
latency reflects queueing, not service.

**Why warm-up.** The first orders pay for page faults, cold caches, branch
predictor training, and the CPU coming out of a low-power state. They are not
representative of steady state, so they are measured and discarded.

**Why 10 repetitions and medians.** A single run on this host can be hit by a
scheduler decision that has nothing to do with the code. The median across
repetitions is robust to that; the p99.9 range column shows how much spread
remains. Crucially, statistics are computed on the 10 run-level values, not on the
1 000 000 orders inside a run, because orders within one run are not independent
samples of the machine's behaviour.

### 5.2 Throughput (unthrottled)

**How.** 2 000 000 orders, client sends batches of 1024 as fast as it can, 5
repetitions. Reported as orders/s plus CPU time per order and context switches.

**Why.** This is the capacity question: with no pacing, what is the steady-state
rate, and what does the CPU have to spend to sustain it? The context-switch
counters are what turn "the mutex is slower" into "the mutex performs 173 000
kernel wakeups per run".

### 5.3 Saturation sweep

**How.** Offered rate stepped 50 k → 6.4 M orders/s plus unthrottled, 3
repetitions each, with the queue's full/empty counters recorded.

**Why.** A queue has two regimes: absorbing bursts (no waits) and being the
bottleneck (waits). The sweep finds where the transition happens for each queue,
and the counters say which side was waiting. This is how the report can state that
the SPSC queue still had headroom at 6.40 M/s while the mutex queue was already
queueing at 6.4 M/s offered.

### 5.4 Component floors

**How.** The four microbenchmarks in §3.14, run alone with no network.

**Why.** To bound explanations. If the book can do 33.68 M ops/s alone (29.7 ns per
op at 2 M ops) and the whole pipeline costs 299 ns per order without a network,
then an end-to-end p50 of 25 µs is overwhelmingly *other things* — arrival,
wakeups, scheduling. Without the floors, any component can be blamed for anything.

### 5.5 Probes and controls

* **Ingress probe** — counts orders already >1 ms old at parse time. This defines
  the clean-run gate.
* **Client-free probe** — the client is allowed to migrate instead of being
  pinned. p99.9 is 63.8 µs free vs 51.5 µs pinned, so client pinning is a
  legitimate control, not a way of hiding client jitter.
* **Network-pinned probe** — pinning the network thread makes p99.9 worse
  (51.5 → 127.5 µs) because a confined thread cannot run away from unrelated work.
* **Host-sensitivity runs** — the same binary and configuration under three host
  conditions.

### 5.6 Correctness gates

Two runs with the same seed must produce the same trade count, quantity and
notional. This is checked across every queue, allocator, scheduling and network
variant: one distinct checksum for latency runs, one for throughput runs. Plus
`verify.sh`: Release tests, ASan/UBSan, TSan on the two queue tests, and an
end-to-end epoll/io_uring parity run. A performance result from a program that
computes different answers is not a result.

---

## 6. How to read the results

The tables below are the ones in `results/summary.md` and
`results/components-summary.md`. For each: what it shows, what it means, and what
it does **not** prove.

### 6.1 The book, alone

| book | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| V1 (map + list + hash) | 11.63 M/s | 81 ns | 420 ns | 595 ns | 10.97 ms |
| V3 (array + slab + bitmap) | 33.68 M/s | 94 ns | 247 ns | 374 ns | 26.7 µs |

**Reads as:** 2.9× the throughput and a 400× smaller worst case, with p50 slightly
*worse* (94 vs 81 ns).

**Why that is not a contradiction.** The median operation is cheap in both: V1's
map insert usually hits a cached node, while V3 pays a little for bitmap
bookkeeping. The difference is 13 ns — below the ~25 ns clock cost both variants
pay in this benchmark. The tail is where the designs differ: V1 occasionally
rebalances a tree or calls `malloc` and lands in the milliseconds; V3's worst case
is bounded by the slab and the bitmap.

**Does not prove:** that V3 is better for every workload. It proves V3 is better
for *this* workload (70 % add, 30 % cancel, fixed price window, bounded order
count) — the limitations are part of the design, not an afterthought.

### 6.2 The pipeline without a network

| queue + book | throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| mutex + V1 | 3.27 M/s | 1.6 µs | 3.6 µs | 6.26 ms |
| mutex + V3 | 3.49 M/s | 1.6 µs | 3.9 µs | 14.6 µs |
| SPSC + V1 | 5.77 M/s | 247 ns | 836 ns | 5.40 ms |
| SPSC + V3 | 7.35 M/s | 299 ns | 1.6 µs | 13.2 µs |

**Reads as:** with no sockets, the SPSC queue cuts p50 from 1.6 µs to 299 ns and
raises throughput 2.1× (7.35 vs 3.49). V1's own tail survives the faster queue:
SPSC + V1 still reaches 5.40 ms at p99.9, because the tree and `malloc` are what
produce it. The queue is not what makes the slow book slow.

**Why this matters:** 299 ns is the floor for any end-to-end number. An integrated
p50 of 25 µs is therefore ~1 % pipeline and ~99 % transport and scheduling. Any
claim that "the engine is slow" has to survive this number.

### 6.3 The allocator, alone

| allocator | alloc ns/op | free ns/op | alloc p99.9 | free p99.9 |
|---|---|---|---|---|
| `operator new`/`delete` | 17.14 | 22.19 | 44 ns | 136 ns |
| slab | 12.59 | 14.13 | 16 ns | 24 ns |

**Reads as:** 1.36× faster allocation, 1.57× faster free, and a much tighter tail.
The absolute numbers are inflated by ~20–25 ns of clock cost per operation, which
both variants pay equally, so the ratio is the meaningful figure.

**Does not prove:** that the allocator explains the end-to-end tail. It cannot —
both allocators are tens of nanoseconds per operation, while the V1 tail is
milliseconds. The allocator is *worst-case insurance*, not the cure.

### 6.4 False sharing, isolated

| layout | throughput (median) | throughput (best) | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| padded | 8.59 M/s | 9.66 M/s | 153 ns | 244 ns | 17.8 µs |
| packed | 10.28 M/s | 11.78 M/s | 197 ns | 530 ns | 26.2 µs |

**Reads as:** padding wins per-item latency in 18 of 18 repetitions across two
independently measured host conditions (two-sided sign test, p ≈ 8×10⁻⁶), and
loses on throughput here in 8 of 9 repetitions.

**Why both can be true.** Latency measures the cost of one hand-off, and false
sharing makes every hand-off do extra cache-coherence work: both threads keep
writing to the same line, so the line bounces between their caches. Throughput on
this desktop measures how long a pair of spinning threads keeps both cores, and
preemption can only *subtract* from that, so the direction is not stable — in the
repeated measurement under contention, padding was ahead in 3 of 9 repetitions.
The plausible mechanism (not verified here) is that packing puts the slot write
and the index publish on one line, which reduces line transfers per item and can
therefore raise the rate, while putting both threads on that same line and
therefore raising the per-item latency.

The honest conclusion is the one in the report: **padding is a latency win and a
throughput trade-off**, and no throughput claim is made. Which number you quote
decides the answer: the throughput medians favour packing (10.28 vs 8.59 M/s),
while p50, p99 and p99.9 favour padding (153 vs 197 ns, 244 vs 530 ns, 17.8 vs
26.2 µs). Reporting one of those as "the result" would have been confident and
incomplete.

### 6.5 Integrated latency: the headline table

| config | p50 | p90 | p99 | p99.9 | clean runs |
|---|---|---|---|---|---|
| SPSC / pinned / epoll | 25.0 µs | 32.6 µs | 37.6 µs | 85.6 µs | 9/10 |
| mutex / pinned / epoll | 27.2 µs | 60.0 µs | 70.9 µs | 105.3 µs | 10/10 |
| SPSC / default / epoll | 26.8 µs | 34.6 µs | 38.9 µs | 127.7 µs | 10/10 |
| mutex / default / epoll | 24.3 µs | 58.3 µs | 71.7 µs | 158.5 µs | 10/10 |
| SPSC / pinned / uring | 24.8 µs | 31.7 µs | 36.7 µs | 96.4 µs | 10/10 |
| mutex / pinned / uring | 29.1 µs | 61.9 µs | 71.1 µs | 113.7 µs | 10/10 |

**Reads as:** p50 is 24–29 µs for *every* configuration. p90 is where the queues
separate: 32.6 vs 60.0 µs. The mutex queue's cost is not in the typical order; it
is in the ones that had to wait for a wakeup.

**Do not read this table as a uniform speed-up.** At p50 the two queues are
statistically indistinguishable (p = 0.145), and the ordering of the medians flips
depending on pinning and backend. What the table supports is narrower: from p90
onward the mutex queue is consistently worse, and its throughput is much lower.

### 6.6 Arrival versus service

| config | total p99.9 | arrival p99.9 | service p99.9 | arrival share |
|---|---|---|---|---|
| SPSC / pinned / epoll | 85.6 µs | 43.9 µs | 23.6 µs | 51 % |
| mutex / pinned / epoll | 105.3 µs | 58.7 µs | 42.0 µs | 56 % |
| SPSC / default / epoll | 127.7 µs | 83.8 µs | 20.7 µs | 66 % |
| mutex / default / epoll | 158.5 µs | 60.2 µs | 102.6 µs | 38 % |

**Reads as:** roughly half of the 99.9th-percentile delay happens *before* the
engine sees the order, in the client → TCP → network-thread path. The service
component is 20.7–42.0 µs.

**Why this is the most important table.** It tells you which component to work on.
For the mutex queue, service p99.9 is 102.6 µs — the queue really is the problem.
For SPSC unpinned, service p99.9 is 20.7 µs and arrival is 83.8 µs — the engine is
fine and the ingress path is being scheduled badly. Two configurations that look
similar in total can need completely different fixes.

### 6.7 Queue: mutex versus lock-free

| comparison | SPSC | mutex | p |
|---|---|---|---|
| p50 | 26.8 µs | 24.3 µs | 0.145 — not distinguishable |
| p99 | 38.9 µs | 71.7 µs | **0.0008** |
| p99, pinned | 38.3 µs | 70.9 µs | **0.0015** |
| service p99.9, pinned | 20.7 µs | 42.0 µs | **0.0008** |
| total p99.9, pinned | 86.8 µs | 105.3 µs | 0.143 — not distinguishable |

**Reads as:** the queue difference is real at p99 and at the service tail, and it
is *not* resolvable at p50 or at the total p99.9 with 10 runs per configuration.

**Why the last row matters.** The total p99.9 is dominated by host noise, so a
real service-level improvement disappears inside it. If the only reported metric
had been total p99.9, the correct conclusion would have been "no measurable
difference" — and that would have been wrong about the mechanism. This is why the
split exists.

**Mechanism, in one line:** the mutex queue turns each hand-off into a kernel
wakeup (157 000–199 000 voluntary switches in an unthrottled run, 12–15 for SPSC),
and at 200 000 orders/s the queue records 18 500–33 200 blocking pops per run,
which is what its empty-wait counter counts.

### 6.8 Allocator, integrated

| book | p50 | p99 | p99.9 |
|---|---|---|---|
| fast-slab | 26.6 µs | 37.5 µs | 98.7 µs |
| fast-heap | 24.5 µs | 35.9 µs | 119.5 µs |
| v1 | 24.4 µs | 36.6 µs | 6.52 ms |

**Reads as:** slab has a smaller p99.9 (98.7 vs 119.5 µs) but a slightly *larger*
p50, and the difference is not significant at n = 10 (p = 0.37).

**The trap in this table:** the `v1` row is not "another allocator". It is a
different book. Its 6.52 ms p99.9 is the tree's malloc/rebalance tail. If you read
that row as "the allocator matters enormously", you have attributed a data-structure
effect to memory. The isolated allocator benchmark (§6.3) is the clean comparison,
and it shows a 1.4–1.6× constant-factor improvement, nothing more.

### 6.9 Scheduling and affinity

| config | p50 | p99 | p99.9 | engine run-queue wait |
|---|---|---|---|---|
| default | 26.8 µs | 38.9 µs | 127.7 µs | 2.2 ms |
| pinned | 25.0 µs | 37.6 µs | 85.6 µs | 0.2 ms |

**Reads as:** pinning cut the engine's run-queue wait by ~10× (2.2 ms → 0.2 ms) and
the total p99.9 by 33 % (127.7 → 85.6 µs), while the median barely moved.

**Why the honest conclusion is "a counter moved, the end-to-end distribution
mostly did not":** the run-queue wait is a direct measurement of "the engine was
runnable but not running", and it dropped by an order of magnitude. But the
end-to-end improvement is not statistically resolvable at n = 10 (p = 0.14),
because the tail is dominated by arrival and by the floating network thread
absorbing interference. Pinning is also not free: the network-pinned probe shows a
confined thread can end up *worse* (51.5 → 127.5 µs) because it cannot migrate away
from unrelated work. Pinning is a bet that the core is quiet. With `isolcpus` it
would be a much better bet; that was not available here.

### 6.10 Network: epoll versus io_uring

| config | p50 | p99 | p99.9 | kernel ops/run |
|---|---|---|---|---|
| epoll | 25.0 µs | 37.6 µs | 85.6 µs | 47 346 |
| io_uring | 24.8 µs | 36.7 µs | 96.4 µs | 31 569 |

**Reads as:** io_uring removed a third of the kernel operations and did not
improve the tail (96.4 vs 85.6 µs, i.e. slightly worse within noise).

**Why that is a legitimate result, not a failed experiment:** the hypothesis was
"fewer syscalls ⇒ lower latency". The measurement says the cost is in wakeups and
data movement, not syscall count. A negative result with a mechanism is more
useful than a positive result without one.

### 6.11 Throughput and saturation

| queue (unthrottled) | orders/s | engine CPU/order | net nvcsw |
|---|---|---|---|
| SPSC, pinned, epoll | 5.86 M/s | 0.17 µs | 12 |
| mutex, pinned, epoll | 3.34 M/s | 0.30 µs | 172 731 |

| queue, offered | achieved | p50 | full waits |
|---|---|---|---|
| SPSC, 6.40 M/s | 6.40 M/s | 10.3 µs | 0 |
| mutex, 6.40 M/s | 3.32 M/s | 32.65 ms | 414 210 |
| SPSC, unthrottled | 5.56 M/s | 12.86 ms | 1 841 316 |
| mutex, unthrottled | 3.33 M/s | 21.48 ms | 249 965 |

**Reads as three separate facts:**

1. **Capacity:** 5.86 vs 3.34 M/s (1.75×). At that rate the mutex path spends
   0.30 µs of engine CPU per order against 0.17 µs, and performs ~173 000 wakeups.
2. **Headroom:** the SPSC queue absorbed 6.40 M/s with *zero* full waits. It was
   not the bottleneck at any rate the client could offer.
3. **Behaviour past capacity:** once the offered rate exceeds what the engine can
   drain, both queues collapse to a queueing regime with double-digit-millisecond
   medians. This is not a defect; it is what a bounded queue does. The difference
   is where the collapse starts and how it looks.

**The nuance worth knowing:** at 6.4 M/s offered, the mutex queue is *bistable* —
2 of 3 repetitions collapsed to ~3.3 M/s with a 32.7 ms median, while 1 of 3
sustained ~6.01 M/s. That is a real property of a wakeup-driven queue near its
limit: whether the producer or the consumer wins the race decides the regime.
Quoting a single median would hide it; the report does not.

**Why "SPSC is 1.75× faster" is not the whole story:** the SPSC path costs a
permanently busy core (101 % engine CPU vs 8 %). If that core is needed for
something else, the mutex queue may be the better engineering choice. The
measurement supports a trade-off, not a verdict.

### 6.12 The host is part of the result

| condition | p50 | p99 | p99.9 | late orders (>1 ms at parse) |
|---|---|---|---|---|
| quiet | 25.0 µs | 37.6 µs | 85.6 µs | 0 |
| desktop confined to E-cores | 22.5 µs | 68 µs | 142 µs | 0 |
| desktop free | 26.0 µs | 1 046 µs | 2 400 µs | thousands |

**Reads as:** the same binary, same configuration, p99.9 moving 17× because of what
else was running, while p50 stays at 22–26 µs.

**The conclusion to carry away:** the median is mostly a property of the code, and
the tail is mostly a property of the machine. The *orderings* between
configurations survive the change of host condition (SPSC over mutex, V3 over V1,
slab over heap, padding over packing on latency), so they are properties of the
code. Absolute tail numbers are not.

### 6.13 Correctness

One distinct checksum across all latency configurations
(`trades=788580 qty=20127113 notional=201270962137`) and one across all throughput
configurations (`trades=1561056 qty=39843705 notional=398436546776`). Every
performance table above is from a program that computed the same answer as the
oracle.

### 6.14 What the p-values mean

The test pools the run-level values from two configurations and asks: if the
labels were meaningless, how often would a split at least this extreme appear?
With n = 10 vs 10 there are C(20,10) = 184 756 splits, so the test is exact.

| result | p | reading |
|---|---|---|
| queue p50 | 0.145 | no evidence of a difference |
| queue p99 | 0.0008 | clear difference |
| queue p99, pinned | 0.0015 | clear difference |
| service p99.9, pinned | 0.0008 | clear difference |
| total p99.9, pinned | 0.143 | no evidence at the total level |
| allocator p99.9 | 0.37 | no evidence |

Two things this buys: nobody can claim a win the data does not support, and a
"no difference" result is publishable rather than embarrassing. With 10 runs the
test is also honest about its own resolution — small effects are simply not
resolvable, and the report says so.

---

## 7. Numbers worth remembering

| number | what it is |
|---|---|
| 200 000 orders/s | the open-loop load every latency number is measured at |
| 1 010 000 / 10 000 | orders sent / warm-up discarded per latency run |
| 25.0 vs 27.2 µs | p50, SPSC vs mutex, pinned, epoll |
| 37.6 vs 70.9 µs | p99, same pair |
| 5.86 vs 3.34 M/s | unthrottled throughput, same pair |
| 0.17 vs 0.30 µs | engine CPU per order, same pair |
| 12–15 vs 157 000–199 000 | network-thread voluntary context switches per throughput run |
| 299 ns | p50 of queue + engine with no sockets (the floor) |
| 13.2 µs | p99.9 of that floor |
| 21.6 of 25.0 µs | arrival share of the median (the network, not the engine) |
| 6.52 ms | V1's p99.9 in the integrated run (the book's malloc tail) |
| 26.7 µs | V3's worst single operation in the book benchmark |
| 6.40 M/s, 0 full waits | what the SPSC queue absorbed before backpressure |
| 2.40 ms → 142 µs | p99.9 change caused only by confining another desktop process |
| 120/122 | latency runs that passed the clean-ingress gate |
| 1 | distinct checksum across every configuration |

---

## 8. Glossary

**Order book** — the set of waiting buy and sell orders, organised by price and
arrival time.

**SPSC** — single producer, single consumer. One thread writes, one thread reads,
nobody else touches the structure.

**Ring buffer** — a fixed-size array with a moving read index and write index that
wrap around; the queue used here.

**Cache line** — the unit the CPU moves between memory and cache (64 bytes here).
Two threads writing to two variables in the same line fight over the line even
though the variables are unrelated.

**False sharing** — that fight. It costs latency without any logical conflict.

**Memory ordering / acquire-release** — the rules for when one thread is guaranteed
to see another thread's writes. A release store publishes everything written
before it; an acquire load makes those writes visible after it.

**CAS** — compare-and-swap, the usual primitive for multi-writer lock-free code.
This project does not need it (D2).

**Backpressure** — what a bounded queue does when full: the producer must wait, and
the offered load can no longer be absorbed.

**Open loop** — the load generator keeps its schedule regardless of the server's
speed, so latency is measured at a known rate.

**Closed loop / unthrottled** — the generator sends as fast as it can, so the rate
is whatever the system can sustain; this measures capacity, not latency.

**Warm-up** — the initial period discarded so page faults, cold caches and CPU
power-state transitions do not contaminate steady-state numbers.

**Run-queue wait** — time a thread was runnable but not running, from
`/proc/thread-self/schedstat`.

**Voluntary / involuntary context switch** — a thread giving up the CPU (blocking)
versus being taken off it (preemption).

**Slab allocator** — pre-allocated pool of fixed-size chunks; allocation is popping
from a free list.

**Intrusive list** — a list whose pointers are stored inside the elements, so no
separate node allocation is needed.

**Open addressing** — a hash table that resolves collisions by probing other slots
in the same array, instead of chaining.

**Bitmap + `ctz`/`clz`** — a bit per price level, and instructions that find the
first set bit, giving "best price" in a few cycles.

**Permutation test** — a significance test that enumerates every relabelling of the
data, used here instead of a formula because n is small and no distribution is
assumed.

**SCHED_FIFO** — the real-time scheduling policy that would let the engine thread
preempt normal work; needs `CAP_SYS_NICE`, which was not granted.

**P-core / E-core** — performance and efficiency cores; the host has 4P + 4E with
SMT on the P-cores, which is why pinning targets CPU 2 (P-core) and avoids CPU 0
and SMT siblings.

**ASan / UBSan / TSan** — sanitizers: memory errors, undefined behaviour, and data
races, respectively.

**Differential testing** — running two implementations on the same input and
requiring identical output; how correctness is defined here (D3).

---

## 9. Where to look next

| if you want… | read |
|---|---|
| the results and their justification | [`../matching-engine-project-doc.md`](../matching-engine-project-doc.md), §6 |
| the IEEE-format version | [`../matching-engine-report.tex`](../matching-engine-report.tex) |
| the interfaces the components were built against | [`../design/component-contract.md`](../design/component-contract.md) |
| why the baseline queue blocks | [`../design/ring-buffer-decision.md`](../design/ring-buffer-decision.md) |
| the build plan followed | [`../plan/iteration-plan.md`](../plan/iteration-plan.md) |
| raw generated tables | [`../../results/summary.md`](../../results/summary.md), [`../../results/components-summary.md`](../../results/components-summary.md) |
| the machine it ran on | [`../../results/env.txt`](../../results/env.txt) |

If you read code in one sitting, use this order — about 1 000 lines, in dependency
order:

```
include/order.hpp                  the data
include/wire.hpp                   bytes -> orders
include/matching_engine.hpp        the rules
include/order_book_fast.hpp        the fast book (skim the bitmap and hash details first)
include/spsc_ring_buffer.hpp       the hand-off
src/main.cpp                       the engine loop and the CSV
```

Everything else is either a control (`order_book.*`, `ring_buffer.*`,
`HeapAllocator`) or scaffolding for measurement (`bench/`, `scripts/`).
