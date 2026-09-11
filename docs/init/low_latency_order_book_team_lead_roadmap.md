# Low-Latency Order Book --- Team Lead Roadmap

> **Purpose of this document:** This is the private execution/learning
> map for the team lead. It is intentionally more detailed than the
> professor/team proposal. You do not need to know all of this before
> starting. The project should be approached as a sequence of
> increasingly difficult problems.

------------------------------------------------------------------------

# 0. First: What Are We Actually Building?

Do not think of the project as:

> "We are making a stock-market simulator."

That framing will send the team toward finance logic and UI.

Think of it as:

> **A concurrent, low-latency event-processing system whose workload is
> an order matching engine.**

The order book is the workload that gives us:

-   strict ordering
-   deterministic correctness
-   lots of small events
-   a naturally latency-sensitive processing path
-   an easy way to generate synthetic traffic

The real learning target is:

``` text
Linux + threads
      ↓
synchronization
      ↓
concurrent queues
      ↓
atomics
      ↓
memory ordering
      ↓
CPU/cache behavior
      ↓
measurement
      ↓
performance engineering
```

------------------------------------------------------------------------

# 1. The Final Architecture You Should Aim For

Start here mentally:

``` text
                         WORKLOAD GENERATORS
                    ┌────────┬────────┬────────┐
                    │        │        │        │
                    ▼        ▼        ▼        ▼
                   P1       P2       P3       P4
                    │        │        │        │
                    ▼        ▼        ▼        ▼
                 Queue1   Queue2   Queue3   Queue4
                    │        │        │        │
                    └────────┴────────┴────────┘
                              │
                              ▼
                     ┌─────────────────┐
                     │ MATCHING ENGINE │
                     │                 │
                     │  one owner      │
                     │  one thread     │
                     └────────┬────────┘
                              │
                 ┌────────────┴────────────┐
                 ▼                         ▼
             Trade events             Statistics
```

But **do not build this all at once**.

Build vertically:

``` text
Stage 1:
Order → Matching Engine

Stage 2:
Producer → Mutex Queue → Engine

Stage 3:
Producer → SPSC Ring → Engine

Stage 4:
Multiple Producers → Multiple Queues → Engine

Stage 5:
Benchmark + latency measurement

Stage 6:
Linux profiling + experiments
```

Every stage must work before the next stage starts.

------------------------------------------------------------------------

# 2. Your Most Important Rule

## Correctness → Measurement → Optimization

Never:

``` text
"I heard lock-free is faster, so let's make it lock-free."
```

Instead:

``` text
Build correct baseline
       ↓
Measure
       ↓
Identify bottleneck
       ↓
Change one thing
       ↓
Measure again
       ↓
Explain result
```

This is the engineering habit the project should teach you.

------------------------------------------------------------------------

# 3. Phase 0 --- Team Setup

Before writing the actual system:

``` text
repo/
├── CMakeLists.txt
├── include/
│   ├── order.hpp
│   ├── order_book.hpp
│   ├── matching_engine.hpp
│   └── ...
├── src/
├── tests/
├── benchmarks/
├── experiments/
├── scripts/
├── docs/
└── README.md
```

Use:

-   C++20
-   CMake
-   Git
-   Linux
-   GCC/Clang
-   warnings enabled

Suggested compiler flags during development:

``` text
-Wall
-Wextra
-Wpedantic
-g
-O0 / -O1 for debugging
```

For actual performance measurements:

``` text
-O2
```

or:

``` text
-O3
```

Do not benchmark debug builds.

------------------------------------------------------------------------

# 4. Phase 1 --- Learn the Domain

You need only a tiny amount of finance knowledge.

## Order

Represent:

``` cpp
struct Order {
    uint64_t id;
    Side side;
    uint64_t price;
    uint64_t quantity;
    OrderType type;
};
```

For the first version:

``` text
BUY
SELL

LIMIT
CANCEL
```

Use integer prices rather than floating-point.

For example:

``` text
₹100.25 → 10025
```

This avoids floating-point equality problems.

------------------------------------------------------------------------

# 5. What Is an Order Book?

Think of it as two collections.

``` text
BUY SIDE

Price  Quantity
105    20
104    15
103     8
```

The best BUY is:

``` text
105
```

because a buyer prefers the highest price.

For SELL:

``` text
SELL SIDE

Price  Quantity
106    10
107    20
108    30
```

The best SELL is:

``` text
106
```

because a seller prefers the lowest price.

A match exists when:

``` text
best_bid >= best_ask
```

Example:

``` text
BUY  105
SELL 103

105 >= 103

→ match
```

------------------------------------------------------------------------

# 6. Price-Time Priority

Suppose:

``` text
BUY 100 qty=10 order_id=1
BUY 100 qty=20 order_id=2
BUY 101 qty=5  order_id=3
```

Order 3 has priority because:

``` text
101 > 100
```

Between:

``` text
BUY 100 qty=10 id=1
BUY 100 qty=20 id=2
```

order 1 has priority because it arrived first.

So each price level needs FIFO behavior.

A conceptual representation:

``` text
BUY

105 → [Order 7]
104 → [Order 2] [Order 4] [Order 8]
103 → [Order 1] [Order 5]
```

Do not prematurely optimize the data structure.

First make this correct.

------------------------------------------------------------------------

# 7. Phase 1 Deliverable

Build:

``` text
Order
OrderBook
MatchingEngine
```

Tests:

``` text
insert buy
insert sell
match
partial match
multiple matches
same-price FIFO
cancel
empty book
```

Example:

``` text
BUY  10 @ 100
SELL 4  @ 99

Expected:

TRADE 4 @ 99

Remaining:

BUY 6 @ 100
```

Write deterministic tests before doing concurrency.

------------------------------------------------------------------------

# 8. Phase 2 --- Understand the Threading Problem

Now imagine four producers:

``` text
P1 ──┐
P2 ──┤
P3 ──┼──> Matching Engine
P4 ──┘
```

If all of them directly modify the order book:

``` text
shared order book
       ↑
   race/locking
```

You need synchronization.

The obvious solution:

``` cpp
std::mutex m;

m.lock();
engine.process(order);
m.unlock();
```

But this creates a serialized critical section.

More importantly, the order book becomes harder to reason about.

------------------------------------------------------------------------

# 9. The Design Decision: Single Owner

Use:

``` text
Producers
    ↓
Queues
    ↓
ONE ENGINE THREAD
    ↓
OrderBook
```

The matching engine owns the order book.

This means:

``` text
OrderBook:
    touched only by engine thread
```

That dramatically simplifies correctness.

This pattern is worth remembering beyond this project:

> **Single ownership can eliminate synchronization rather than trying to
> make shared state perfectly synchronized.**

------------------------------------------------------------------------

# 10. Phase 2 --- Mutex Baseline

Before lock-free code, build the boring implementation.

For example:

``` text
Producer
   ↓
std::queue
   ↓
mutex
   ↓
Engine
```

Conceptually:

``` cpp
{
    std::lock_guard<std::mutex> lock(mutex);
    queue.push(order);
}
```

Consumer:

``` cpp
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!queue.empty()) {
        order = queue.front();
        queue.pop();
    }
}
```

Add a condition variable if needed so the consumer can sleep when no
work exists.

This is your baseline.

Do not skip it.

Without the baseline, later performance claims mean very little.

------------------------------------------------------------------------

# 11. What You Need to Learn About Threads

Before proceeding, understand:

``` text
thread
mutex
critical section
condition variable
race condition
deadlock
livelock
contention
context switch
```

You do not need advanced kernel scheduling theory yet.

The basic mental model:

``` text
CPU core
   ↑
scheduler
   ↑
runnable threads
```

The OS decides when threads run.

If many threads fight over a lock, the performance cost isn't simply
"the mutex takes X nanoseconds."

The entire execution pattern can change.

------------------------------------------------------------------------

# 12. Phase 3 --- Ring Buffer

Now learn the data structure.

A fixed-size ring:

``` text
capacity = 8

       0   1   2   3   4   5   6   7
      ┌───┬───┬───┬───┬───┬───┬───┬───┐
      │ A │ B │ C │ D │   │   │   │   │
      └───┴───┴───┴───┴───┴───┴───┴───┘
        ↑           ↑
       read        write
```

When:

``` text
write == capacity
```

wrap:

``` text
write = 0
```

A simple implementation can use:

``` cpp
index % capacity
```

Later, if capacity is a power of two:

``` text
index & (capacity - 1)
```

can avoid modulo, although you should **measure before caring about
this**.

------------------------------------------------------------------------

# 13. SPSC First

SPSC:

``` text
Single Producer
Single Consumer
```

Architecture:

``` text
Producer
   │
   ▼
┌────────────────┐
│ SPSC Ring      │
│                │
│ write index →  │
│ read index  →  │
└────────────────┘
   │
   ▼
Consumer
```

The producer owns the write position.

The consumer owns the read position.

That ownership is what makes the algorithm much simpler.

------------------------------------------------------------------------

# 14. Why Atomics?

Imagine:

``` text
Producer:
write_index = 10;

Consumer:
reads write_index
```

These operations happen on different CPU cores.

You need a defined way for threads to coordinate.

Use:

``` cpp
std::atomic<size_t>
```

Now learn:

``` text
atomic load
atomic store
memory_order_relaxed
memory_order_acquire
memory_order_release
memory_order_seq_cst
```

Do not start by memorizing definitions.

Start with this question:

> What information must become visible to the other thread, and at what
> point?

------------------------------------------------------------------------

# 15. The Acquire/Release Mental Model

Imagine producer does:

``` text
write order into slot
       ↓
publish new write index
```

Consumer does:

``` text
observe published write index
       ↓
read order from slot
```

The important guarantee is:

``` text
producer writes order
       ↓
release
       ↓
consumer acquire
       ↓
consumer sees order
```

This is the core idea.

`release` publishes previous writes.

`acquire` allows the consumer to safely observe what was published.

The C++ memory model is subtle. Use `std::memory_order_seq_cst` first if
necessary for a simple correct prototype, then study why acquire/release
is sufficient for the specific queue.

A good reference is cppreference's `std::memory_order` documentation.

------------------------------------------------------------------------

# 16. Important Warning About Lock-Free Code

Do not copy a random lock-free queue from GitHub.

You need to understand:

``` text
who owns each variable?
who writes it?
who reads it?
when is a slot considered available?
when is it considered published?
what prevents overwrite?
what prevents reading an uninitialized slot?
```

Draw the state machine first.

For each queue slot:

``` text
EMPTY
  ↓
WRITTEN
  ↓
CONSUMED
  ↓
EMPTY
```

Then reason about the producer and consumer transitions.

------------------------------------------------------------------------

# 17. Phase 4 --- Multiple Producers

Do **not** immediately write an MPSC queue.

Use:

``` text
P1 → SPSC Q1 ──┐
P2 → SPSC Q2 ──┤
P3 → SPSC Q3 ──┼──> Engine
P4 → SPSC Q4 ──┘
```

The engine can poll queues.

Example:

``` text
while (running) {

    check Q1
    check Q2
    check Q3
    check Q4

    process available orders
}
```

Now you have a real concurrency architecture without implementing one of
the hardest classes of lock-free queues.

If the team finishes early, MPSC can become a stretch goal.

------------------------------------------------------------------------

# 18. Ordering Problem

This is a subtle point.

If multiple producers exist:

``` text
P1 → Q1
P2 → Q2
```

what does "first" mean?

Suppose:

``` text
P1 generated order A
P2 generated order B
```

but the engine observes:

``` text
B then A
```

You need a defined ordering model.

For the project, the cleanest solution is to assign an event sequence
number at ingestion or define ordering according to when the engine
consumes events.

Document this explicitly.

Do not accidentally claim to reproduce real exchange ordering.

------------------------------------------------------------------------

# 19. Phase 5 --- Benchmarking

Now the project becomes interesting.

You need a workload generator.

Example:

``` text
1,000,000 orders
```

with controlled distributions:

``` text
BUY/SELL ratio
price distribution
quantity distribution
cancel rate
```

Use deterministic random seeds so experiments can be reproduced.

For every experiment record:

``` text
commit hash
machine
CPU
compiler
optimization flags
thread count
queue size
workload size
result
```

This is extremely important.

------------------------------------------------------------------------

# 20. Latency Measurement

Avoid:

``` text
printf("time = ...")
```

inside the hot path.

That destroys your measurement.

Instead:

``` text
timestamp before submission
        ↓
queue
        ↓
engine
        ↓
timestamp after processing
```

Store measurements in memory.

After the experiment:

``` text
sort latencies
calculate percentiles
```

At minimum:

``` text
min
p50
p95
p99
p99.9
max
```

Also calculate:

``` text
throughput = processed events / elapsed time
```

------------------------------------------------------------------------

# 21. Timestamping: Don't Overcomplicate It Initially

Use:

``` cpp
std::chrono::steady_clock
```

for the initial implementation.

Only investigate:

``` text
RDTSC / RDTSCP
```

later if you have a reason.

CPU timestamp counters introduce architecture-specific concerns such as:

-   serialization
-   core migration
-   invariant TSC
-   measurement overhead

You do not need that complexity to produce a strong project.

If you later study it, treat it as an experiment.

------------------------------------------------------------------------

# 22. Phase 6 --- Benchmark Matrix

Build a table like:

``` text
                 Throughput   p50   p99   p99.9
1 producer
2 producers
4 producers
8 producers
```

For:

``` text
mutex design
SPSC design
```

Then graph:

``` text
throughput vs producers
p99 latency vs producers
```

The interesting part is not whether your graph looks good.

The interesting part is explaining it.

------------------------------------------------------------------------

# 23. What Results Might Look Like?

You might observe:

``` text
1 producer:
mutex       2.0M/s
ring        2.2M/s

4 producers:
mutex       2.1M/s
ring        3.8M/s

8 producers:
mutex       1.7M/s
ring        3.5M/s
```

Possible explanation:

``` text
more producers
      ↓
more contention
      ↓
mutex waiting
      ↓
higher latency
```

But do not write that explanation until measurements support it.

Use profiling.

------------------------------------------------------------------------

# 24. Throughput vs Latency

This distinction is essential.

Throughput:

``` text
How much work per second?
```

Latency:

``` text
How long did one event take?
```

You can have:

``` text
high throughput
+
bad tail latency
```

For example:

``` text
p50   = 5 µs
p99   = 20 µs
p99.9 = 900 µs
```

That means most events are fast, but a small fraction are dramatically
slower.

This is why the project should report distributions rather than only
averages.

------------------------------------------------------------------------

# 25. Phase 7 --- Linux CPU Affinity

Now introduce Linux.

Normally the OS scheduler decides where a thread executes.

You can restrict a thread to specific CPUs.

Conceptually:

``` text
Without affinity:

Thread → CPU 0/1/2/3 ...

With affinity:

Thread → CPU 2
```

Study:

``` text
sched_setaffinity()
```

Run:

``` text
unbound
vs
pinned
```

and measure.

Do not assume pinning improves performance.

Sometimes it can hurt.

Again:

``` text
hypothesis → measurement → explanation
```

------------------------------------------------------------------------

# 26. Phase 8 --- perf

Now use Linux profiling.

Useful commands to learn:

``` bash
perf stat ./benchmark
```

and:

``` bash
perf record ./benchmark
perf report
```

`perf stat` can provide high-level hardware/software performance
counters.

Depending on your CPU and permissions, investigate:

``` text
cycles
instructions
context switches
cache references
cache misses
branch misses
```

Don't obsess over every counter.

The objective is:

``` text
benchmark result
      ↓
profiling evidence
      ↓
engineering explanation
```

------------------------------------------------------------------------

# 27. Example Profiling Question

Suppose:

``` text
8 producers
```

causes a huge p99 spike.

Don't immediately rewrite the queue.

Ask:

``` text
Did context switches increase?

Did CPU utilization change?

Did cache misses increase?

Did the engine become queue-starved?

Did the queue become saturated?

Did producer contention increase?
```

Then investigate.

This is the actual performance-engineering skill you want to learn.

------------------------------------------------------------------------

# 28. False Sharing

Only after everything works.

Imagine:

``` cpp
struct Queue {
    atomic<size_t> read;
    atomic<size_t> write;
};
```

If `read` and `write` land on the same cache line:

``` text
CACHE LINE
┌──────────────────────────┐
│ read │ write │ ........  │
└──────────────────────────┘
```

different CPUs may repeatedly modify the same cache line.

Even though they are modifying different variables.

This can create unnecessary cache-coherence traffic.

A later experiment can separate them:

``` text
CACHE LINE 1
┌──────────────────────────┐
│ read                     │
└──────────────────────────┘

CACHE LINE 2
┌──────────────────────────┐
│ write                    │
└──────────────────────────┘
```

Measure before and after.

Do not blindly add padding everywhere.

------------------------------------------------------------------------

# 29. Cache Locality

Your ring buffer is deliberately contiguous:

``` text
[event][event][event][event]
```

Compare that conceptually with a linked structure:

``` text
event → random memory
          ↓
event → another location
          ↓
event → another location
```

The ring buffer is friendly to spatial locality.

This is one reason low-level data structures matter.

But again:

> "contiguous is faster"

is a hypothesis.

Benchmark it.

------------------------------------------------------------------------

# 30. Memory Allocation

Do not dynamically allocate an object for every order in the hot path if
you can avoid it.

Bad direction:

``` text
new Order
new Order
new Order
new Order
...
```

Better:

``` text
preallocated event storage
```

Why?

Because allocation can introduce:

-   allocator overhead
-   contention
-   unpredictable latency
-   cache effects

A simple object representation plus a fixed-capacity queue is enough.

A custom allocator is **not required**.

If you finish early, a memory pool can be a stretch goal.

------------------------------------------------------------------------

# 31. Testing Strategy

You need three kinds of tests.

## A. Deterministic correctness tests

Example:

``` text
BUY 10 @ 100
SELL 5 @ 99

Expected:
trade 5 @ 99
remaining BUY = 5
```

## B. Randomized tests

Generate many random orders and compare your implementation against a
simple reference implementation.

This is extremely valuable.

``` text
Reference engine
        vs
Optimized engine
```

Both should produce equivalent results.

## C. Concurrency stress tests

Run:

``` text
many producers
millions of events
many iterations
```

and verify:

``` text
no lost events
no duplicated events
no corrupted orders
correct final state
```

------------------------------------------------------------------------

# 32. Sanitizers

Run development builds with:

``` text
AddressSanitizer
UndefinedBehaviorSanitizer
ThreadSanitizer
```

AddressSanitizer catches memory errors such as out-of-bounds access and
use-after-free.

ThreadSanitizer is especially relevant for this project because it
detects data races.

Do not benchmark sanitizer builds.

They are for correctness/debugging.

------------------------------------------------------------------------

# 33. Important Lock-Free Testing Principle

A program passing:

``` text
100 orders
```

means almost nothing.

Stress it with:

``` text
10 million+
```

events.

Vary:

``` text
queue capacity
producer count
workload
CPU placement
```

Run repeatedly.

Concurrency bugs can be timing-dependent.

------------------------------------------------------------------------

# 34. Suggested Team Organization

You have five people.

Do not create five independent projects.

Use subsystem ownership.

``` text
                    TEAM LEAD
                       │
        ┌──────────────┼──────────────┐
        │              │              │
 Matching          Concurrency      Benchmark
 Engine             / Queues        / Analysis
        │              │              │
        └──────────────┼──────────────┘
                       │
                 Integration
                       │
                   Testing
```

Suggested ownership:

### Person A --- Order Book

Responsible for:

``` text
Order
PriceLevel
OrderBook
MatchingEngine
```

### Person B --- Queue

Responsible for:

``` text
MutexQueue
SPSCRingBuffer
atomic indexes
stress tests
```

### Person C --- Benchmarking

Responsible for:

``` text
workload generator
timing
percentiles
throughput
experiment runner
```

### Person D --- Linux/Performance

Responsible for:

``` text
perf
CPU affinity
context-switch experiments
cache experiments
profiling
```

### Person E --- Integration/Test/Visualization

Responsible for:

``` text
CI
sanitizers
reference implementation
integration tests
graphs
report tooling
```

You should still understand every subsystem.

------------------------------------------------------------------------

# 35. Suggested 8-Week Schedule

## Week 1 --- Domain + baseline

Learn:

``` text
order book
price-time priority
basic C++ threading
```

Build:

``` text
single-threaded matching engine
```

Deliverable:

``` text
all deterministic tests passing
```

------------------------------------------------------------------------

## Week 2 --- Threading

Learn:

``` text
threads
mutex
condition variable
critical sections
race conditions
```

Build:

``` text
producer → mutex queue → engine
```

Deliverable:

``` text
correct concurrent baseline
```

------------------------------------------------------------------------

## Week 3 --- Ring Buffer

Learn:

``` text
circular buffers
producer/consumer ownership
atomics
```

Build:

``` text
producer → SPSC queue → engine
```

Initially keep memory ordering conservative.

Deliverable:

``` text
SPSC implementation + stress tests
```

------------------------------------------------------------------------

## Week 4 --- Correctness + Multiple Producers

Build:

``` text
P1 → Q1
P2 → Q2
P3 → Q3
P4 → Q4
       ↓
     Engine
```

Define event ordering.

Run heavy stress tests.

Deliverable:

``` text
stable multi-producer architecture
```

------------------------------------------------------------------------

## Week 5 --- Benchmarking

Build:

``` text
workload generator
latency collector
percentile calculation
throughput measurement
```

Benchmark:

``` text
mutex
vs
ring
```

Deliverable:

``` text
first serious benchmark report
```

------------------------------------------------------------------------

## Week 6 --- Linux Performance

Learn:

``` text
perf
CPU affinity
context switches
cache behavior
```

Experiments:

``` text
CPU pinning
producer scaling
queue saturation
```

Deliverable:

``` text
profiling evidence + explanations
```

------------------------------------------------------------------------

## Week 7 --- Optimization

Only now consider:

``` text
memory ordering
cache-line padding
batching
allocation reduction
data-layout changes
```

Each optimization must have:

``` text
before
change
after
explanation
```

Deliverable:

``` text
optimized version + benchmark comparison
```

------------------------------------------------------------------------

## Week 8 --- Finalization

Finish:

``` text
stress tests
graphs
documentation
architecture diagram
report
demo
```

Prepare a demonstration where you can show:

``` text
baseline
    ↓
bottleneck
    ↓
optimization
    ↓
measurement
```

------------------------------------------------------------------------

# 36. What You Should Learn, In Order

Do not randomly consume systems content.

Use this progression:

``` text
1. C++ threads
       ↓
2. mutex + condition_variable
       ↓
3. producer/consumer problem
       ↓
4. ring buffers
       ↓
5. std::atomic
       ↓
6. acquire/release
       ↓
7. CPU caches + cache coherence
       ↓
8. false sharing
       ↓
9. Linux scheduling
       ↓
10. CPU affinity
       ↓
11. perf
       ↓
12. latency analysis
```

You don't need to master all of a topic before implementing the next
component.

Learn just enough to build the current stage, then deepen understanding
through experiments.

------------------------------------------------------------------------

# 37. Learning Resources

## C++ atomics

Use cppreference as the reference once you understand the basic problem:

-   `std::atomic`
-   `std::memory_order`

https://en.cppreference.com/w/cpp/atomic/atomic

https://en.cppreference.com/w/cpp/atomic/memory_order

Do not start by reading the entire memory-model specification.

------------------------------------------------------------------------

## ThreadSanitizer

Clang documentation:

https://clang.llvm.org/docs/ThreadSanitizer.html

Learn to compile a concurrency build with:

``` text
-fsanitize=thread
```

Use this during testing, not benchmarking.

------------------------------------------------------------------------

## AddressSanitizer

https://clang.llvm.org/docs/AddressSanitizer.html

Use it for memory bugs.

------------------------------------------------------------------------

## Linux perf

Start with:

``` bash
perf stat ./benchmark
perf record ./benchmark
perf report
```

Then read the Linux kernel perf documentation:

https://docs.kernel.org/perf/

You do not need to become a perf expert.

You need to become comfortable asking:

> "What evidence can I collect about why this program is slow?"

------------------------------------------------------------------------

## Linux CPU affinity

Read:

``` text
sched_setaffinity(2)
```

https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html

Understand what CPU affinity does before putting it into the benchmark.

------------------------------------------------------------------------

# 38. Books / Deeper Resources

You do not need to read these cover to cover.

### Computer Systems: A Programmer's Perspective

Excellent for:

``` text
memory
CPU
concurrency
machine-level behavior
```

Use selected chapters.

### Operating Systems: Three Easy Pieces

Excellent for:

``` text
threads
concurrency
scheduling
virtual memory
```

https://pages.cs.wisc.edu/\~remzi/OSTEP/

### C++ Concurrency in Action

Useful once basic threading makes sense.

Do not start here if atomics are currently completely unfamiliar.

### Mechanical Sympathy

Useful later for understanding:

``` text
CPU caches
memory barriers
latency
data structures
```

Treat it as supplemental rather than your primary textbook.

------------------------------------------------------------------------

# 39. What You Should NOT Learn Yet

Avoid getting distracted by:

``` text
NUMA
kernel development
DPDK
FPGA
RDMA
custom TCP
lock-free MPMC algorithms
hazard pointers
epoch reclamation
RCU
assembly optimization
```

These are interesting.

They are not required to make this project excellent.

If the team finishes early, choose one extension based on what your
measurements suggest.

------------------------------------------------------------------------

# 40. The Most Important Concept: Ownership

A lot of the project can be understood through ownership.

Ask for every piece of data:

``` text
Who writes this?
Who reads this?
Can multiple threads modify it?
If yes, why?
Can ownership be assigned to one thread instead?
```

For example:

``` text
OrderBook
    ↓
Engine owns it
    ↓
No lock needed
```

Queue:

``` text
write_index
    ↓
producer owns writes

read_index
    ↓
consumer owns writes
```

This is a much better way to reason about concurrent systems than simply
asking:

> "Where do I put the mutex?"

------------------------------------------------------------------------

# 41. The Second Important Concept: Backpressure

Imagine:

``` text
Producer rate = 10M events/sec
Engine rate   = 5M events/sec
```

Then:

``` text
queue:
100
200
500
1000
...
FULL
```

Something must happen.

Options:

``` text
drop
block
reject
overwrite
slow producers
increase capacity
batch processing
```

This is a real systems problem.

Document your policy.

For a correctness-focused project, **dropping or silently overwriting
orders is unacceptable** unless explicitly defined as a benchmark mode.

------------------------------------------------------------------------

# 42. The Third Important Concept: Tail Latency

Suppose:

``` text
average = 10 µs
```

Sounds excellent.

But:

``` text
p99.9 = 1 ms
```

means some events are 100× slower.

Investigate why.

Potential causes could include:

``` text
scheduler activity
contention
cache misses
queueing
OS interruptions
measurement artifacts
```

Do not assume which one.

This is where Linux profiling becomes useful.

------------------------------------------------------------------------

# 43. The Fourth Important Concept: Lock-Free Is Not Magic

You should be able to explain this to the team:

``` text
mutex:
    simpler
    easier to reason about
    may block
    can contend

lock-free:
    avoids traditional blocking
    uses atomics
    harder to implement correctly
    can still have contention
    can still perform badly
```

Therefore the project's question is not:

> "Can we make it lock-free?"

It is:

> "Under what workload and system conditions does the alternative
> synchronization design behave differently, and why?"

------------------------------------------------------------------------

# 44. What a Strong Final Demo Looks Like

Do not spend your final presentation explaining every class.

Instead:

### Part 1

Show the order book.

``` text
BUY 100
SELL 99

→ trade
```

### Part 2

Show architecture.

``` text
producers → queues → engine
```

### Part 3

Show baseline.

``` text
mutex queue
```

### Part 4

Show alternative.

``` text
SPSC ring queues
```

### Part 5

Show benchmark.

``` text
throughput
p50
p99
p99.9
```

### Part 6

Show a surprising result.

For example:

``` text
"Adding more producers stopped increasing throughput."
```

Then show:

``` text
perf
↓
contention / CPU behavior
↓
explanation
```

### Part 7

Show optimization.

``` text
before → after
```

This tells a much better engineering story.

------------------------------------------------------------------------

# 45. Questions Your Professor/TAs May Ask

Prepare for:

### "Why is the matching engine single-threaded?"

Answer:

Because the order book is shared mutable state with strict ordering
requirements. Giving one thread ownership removes synchronization from
the critical state-management path and provides deterministic
processing.

### "Why not simply use a mutex?"

Answer:

We do use one as a baseline. The point is to experimentally compare it
with a queue-based design.

### "Why SPSC instead of MPSC?"

Answer:

SPSC has simpler ownership and synchronization semantics. Multiple SPSC
queues can provide multi-producer ingestion without immediately
introducing the complexity of a general MPSC lock-free algorithm.

### "Why not make the matching engine multithreaded?"

Answer:

The engine's state has ordering dependencies. Parallelizing it safely is
a separate research problem. We chose to parallelize ingestion first so
that concurrency can be studied without compromising the correctness of
the core matching algorithm.

### "Why is this an OS project?"

Answer:

The project directly studies threads, synchronization, atomics,
scheduling, CPU affinity, memory/cache behavior, context switching and
Linux performance analysis using a real workload.

------------------------------------------------------------------------

# 46. Things You Should Be Able to Explain by the End

You should personally be able to draw this:

``` text
Application
    │
    ▼
Producer threads
    │
    ▼
Concurrent queue
    │
    ├── atomic state
    │
    ├── memory ordering
    │
    ▼
Engine thread
    │
    ▼
Order book
    │
    ▼
Trade
```

And explain every transition.

More importantly, you should be able to answer:

> Why did we choose this architecture?

> What happens if the producer is faster than the consumer?

> Where can a race occur?

> Why is this atomic?

> What does acquire/release guarantee here?

> Why did p99 change?

> What evidence says the optimization helped?

If you can answer those without memorized buzzwords, the project
succeeded.

------------------------------------------------------------------------

# 47. Final Scope Hierarchy

Keep this hierarchy visible throughout the project.

## MUST HAVE

``` text
✓ correct order book
✓ matching
✓ cancellation
✓ single-threaded engine
✓ mutex baseline
✓ SPSC ring buffer
✓ multiple producers through multiple queues
✓ correctness tests
✓ stress tests
✓ throughput benchmark
✓ latency percentiles
```

## SHOULD HAVE

``` text
✓ perf profiling
✓ CPU affinity experiment
✓ context-switch analysis
✓ cache-related analysis
✓ reproducible benchmark framework
✓ visualization
```

## NICE TO HAVE

``` text
○ cache-line padding
○ optimized memory ordering
○ batching
○ memory pool
○ MPSC queue
○ network ingestion
```

## DO NOT NEED

``` text
✗ NUMA
✗ kernel module
✗ FPGA
✗ DPDK
✗ custom TCP
✗ distributed exchange
✗ production-grade trading system
```

------------------------------------------------------------------------

# 48. Your First 7 Days

If you are starting tomorrow, do exactly this.

### Day 1

Everyone understands:

``` text
order
bid
ask
limit order
matching
price-time priority
```

You personally learn:

``` text
producer/consumer
mutex
thread
```

### Day 2

Implement:

``` text
Order
```

### Day 3

Implement:

``` text
OrderBook
```

### Day 4

Implement:

``` text
MatchingEngine
```

### Day 5

Write deterministic tests.

### Day 6

Generate random orders and manually verify tricky cases.

### Day 7

Freeze the single-threaded API.

Only then move to concurrency.

------------------------------------------------------------------------

# 49. Your Personal Learning Strategy

You currently have shallow knowledge of some of these concepts. That is
fine.

Do not attempt:

``` text
"learn lock-free programming"
```

as a prerequisite.

Instead:

``` text
Need queue
   ↓
learn ring buffer
   ↓
need producer/consumer
   ↓
learn atomics
   ↓
need ordering
   ↓
learn acquire/release
   ↓
performance weird
   ↓
learn cache coherence
   ↓
latency weird
   ↓
learn scheduling/perf
```

The project itself should determine what you learn next.

That is the best way to avoid drowning in systems theory.

------------------------------------------------------------------------

# 50. The Mental Model to Keep

The entire project can be reduced to this:

``` text
             MANY THREADS
                  │
                  │ events
                  ▼
          ┌───────────────┐
          │  QUEUE LAYER  │
          └───────┬───────┘
                  │
                  │ ordered events
                  ▼
          ┌───────────────┐
          │ ENGINE THREAD │
          └───────┬───────┘
                  │
                  ▼
             ORDER BOOK
                  │
                  ▼
              MATCHING
                  │
                  ▼
              TRADE EVENT


And around all of it:

        ┌──────────────────────────┐
        │ Linux                    │
        │                          │
        │ Scheduler                │
        │ Threads                  │
        │ Atomics                  │
        │ CPU affinity             │
        │ Cache / memory           │
        │ Performance counters     │
        └──────────────────────────┘
```

You are not trying to prove that you can write the world's fastest order
book.

You are trying to become capable of looking at a concurrent system and
asking:

> **Where is the work happening, who owns the state, where can threads
> interfere with each other, what does the OS/hardware do underneath us,
> and how can we prove what is actually happening?**

That is the transferable skill this project should give you.
