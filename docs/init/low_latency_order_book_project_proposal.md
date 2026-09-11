# Low-Latency Event Processing & Order Matching Engine

## 1. Project Overview

### Proposed title

**Low-Latency Event Processing and Order Matching Engine**

### One-line description

We propose to build a C++20, Linux-based order matching engine that
processes a stream of buy/sell orders, with a focus on
**operating-system concepts, concurrency, synchronization, memory
behavior, and performance measurement**.

The project will begin with a simple, correct single-threaded matching
engine. We will then build concurrent ingestion around it and
experimentally compare a conventional mutex-based design with a
low-overhead queue-based design. The main objective is not to simulate a
stock market, but to understand how OS and hardware behavior affects a
latency-sensitive application.

------------------------------------------------------------------------

## 2. Motivation

Many real systems have a similar structure:

> many producers generate events → events are queued → a processing
> component consumes them → results are produced.

Examples include trading systems, telemetry pipelines, network
processing, game servers, messaging systems, and backend infrastructure.

An order matching engine gives us a concrete workload with strict
ordering and low-latency requirements. This allows us to study OS
concepts through an actual system rather than implementing isolated
textbook simulations.

The project will investigate questions such as:

-   How does thread contention affect latency?
-   What is the cost of mutex-based synchronization?
-   How can multiple threads communicate efficiently?
-   What are atomic operations and memory ordering?
-   Why can memory layout affect performance?
-   What is false sharing?
-   How do CPU affinity and scheduling affect latency?
-   Why can average latency look good while tail latency is poor?
-   How can Linux profiling tools explain observed performance?

------------------------------------------------------------------------

## 3. What Is an Order Book?

An order book stores outstanding buy and sell orders.

For example:

``` text
SELL SIDE
Price       Quantity
103         20
102         15
101          8
--------------------
100
--------------------
 99         10
 98         20
 97         40
BUY SIDE
```

A buy order says:

> "I am willing to buy up to this quantity at this price."

A sell order says:

> "I am willing to sell up to this quantity at this price."

If a new order crosses an existing order, the matching engine creates a
trade.

Example:

``` text
Existing:
SELL 5 @ 101

New:
BUY  5 @ 102

Result:
TRADE 5 @ 101
```

For the project, we will support a deliberately limited order model:

-   Limit buy orders
-   Limit sell orders
-   Order cancellation
-   Unique order IDs
-   Quantity
-   Price
-   Deterministic matching

We will follow **price-time priority**:

1.  Better price gets priority.
2.  At the same price, the older order gets priority.

We will not attempt to implement a complete exchange.

------------------------------------------------------------------------

## 4. Project Objectives

The project has four main objectives.

### Objective 1 --- Build a correct matching engine

Implement the order book and matching logic first, independently of
concurrency.

### Objective 2 --- Study concurrent event ingestion

Allow multiple producer threads to generate or receive orders and
deliver them to a single matching-engine thread.

### Objective 3 --- Compare synchronization designs

Implement at least two designs:

1.  A conventional mutex/condition-variable based design.
2.  A low-overhead queue-based design using a fixed-size ring buffer and
    C++ atomics.

The goal is to compare them experimentally rather than assuming that one
is always faster.

### Objective 4 --- Investigate Linux and hardware effects

Measure and analyze:

-   throughput
-   p50 latency
-   p95 latency
-   p99 latency
-   p99.9 latency
-   CPU utilization
-   context switches
-   cache behavior where measurable
-   effect of CPU affinity
-   effect of contention
-   queue saturation

------------------------------------------------------------------------

## 5. High-Level Architecture

The proposed architecture is:

``` text
                 ORDER PRODUCERS
             ┌────────┬────────┬────────┐
             │        │        │        │
             ▼        ▼        ▼        ▼
           P1       P2       P3       P4
             │        │        │        │
             └────────┴────────┴────────┘
                         │
                  Event Queues
                         │
                         ▼
                ┌─────────────────┐
                │ Matching Engine │
                │   Single Core   │
                └────────┬────────┘
                         │
                         ▼
                  Trade / Event
                     Output
```

The matching engine will initially be single-threaded.

This is intentional: the order book itself is difficult to make
concurrent while preserving deterministic matching semantics. Instead of
putting locks inside the core algorithm, we will keep ownership of the
order book with one thread and study the concurrency problem at the
ingestion boundary.

------------------------------------------------------------------------

## 6. Why a Single Matching-Engine Thread?

A naive design might allow several threads to directly modify the order
book:

``` text
Thread 1 ──┐
Thread 2 ──┤
Thread 3 ──┼──> shared order book
Thread 4 ──┘
```

This introduces synchronization into the most latency-sensitive part of
the application.

Our initial design is:

``` text
Producers
   │
   ▼
Queue(s)
   │
   ▼
ONE matching-engine thread
   │
   ▼
Order book
```

Only the engine thread modifies the order book.

Advantages:

-   simpler correctness
-   deterministic event ordering
-   fewer locks around core state
-   easier profiling
-   easier reasoning about races

This is an important systems-design lesson: **more threads do not
automatically mean more performance**.

------------------------------------------------------------------------

## 7. Ring Buffer

A ring buffer is a fixed-size circular array.

For example:

``` text
             write
               ↓
     ┌───┬───┬───┬───┬───┬───┐
     │ A │ B │ C │ D │   │   │
     └───┴───┴───┴───┴───┴───┘
       ↑
      read
```

When the end is reached, the index wraps back to the beginning.

The queue does not repeatedly allocate and deallocate nodes. Instead, it
reuses preallocated storage.

For the first lock-free implementation, we will use an **SPSC ring
buffer** where SPSC means:

> Single Producer, Single Consumer.

The producer writes events and the consumer reads them.

We may later extend the architecture to multiple producer queues rather
than immediately implementing a difficult general MPSC queue.

Conceptually:

``` text
Producer 1 → SPSC Queue 1 ──┐
Producer 2 → SPSC Queue 2 ──┤
Producer 3 → SPSC Queue 3 ──┼──> Matching Engine
Producer 4 → SPSC Queue 4 ──┘
```

This allows us to study concurrency without making the entire project
depend on a highly complex lock-free algorithm.

------------------------------------------------------------------------

## 8. What Does "Lock-Free" Mean Here?

A lock-based queue might do:

``` cpp
lock(mutex);
queue.push(order);
unlock(mutex);
```

A low-overhead queue can instead use atomic indexes:

``` text
producer → atomic write position
consumer → atomic read position
```

The exact memory-ordering rules will be studied and implemented
carefully.

We will initially use the safest understandable design and only weaken
memory ordering after correctness has been established.

This project will therefore teach:

-   `std::atomic`
-   atomic load/store
-   acquire/release semantics
-   sequential consistency
-   happens-before relationships
-   data races
-   synchronization

We will not claim that "lock-free = always faster." The purpose is to
measure the trade-off.

------------------------------------------------------------------------

## 9. Latency Measurement

For a latency-sensitive system, throughput alone is insufficient.

For each event, we will conceptually measure:

``` text
event generated
      │
      ▼
timestamp T1
      │
   queueing
      │
      ▼
matching begins
      │
      ▼
matching completes
      │
      ▼
timestamp T2

latency = T2 - T1
```

We will collect a large number of observations and report distributions
such as:

``` text
p50     median
p95     95% of operations are faster than this
p99     99% are faster than this
p99.9   99.9% are faster than this
```

This allows us to investigate **tail latency**, where a small fraction
of operations may experience substantially worse delays.

------------------------------------------------------------------------

## 10. Experiments

The project will be driven by experiments rather than simply
implementation.

### Experiment 1 --- Single-threaded baseline

Measure the pure matching engine.

Purpose:

-   establish baseline throughput
-   establish baseline latency
-   identify the cost of matching itself

### Experiment 2 --- Mutex-based ingestion

Multiple producers send orders through a conventional synchronized
queue.

Measure:

-   throughput
-   latency
-   context switches
-   behavior as producer count increases

### Experiment 3 --- SPSC queue

Replace the synchronized queue with an SPSC ring buffer.

Compare against the mutex design.

### Experiment 4 --- Producer scaling

Run:

``` text
1 producer
2 producers
4 producers
8 producers
```

and observe where performance stops scaling.

### Experiment 5 --- Queue saturation

Generate events faster than the engine can consume them.

Observe:

-   queue depth
-   latency growth
-   dropped/rejected events if the queue becomes full

### Experiment 6 --- CPU affinity

Run the engine:

``` text
without explicit affinity
```

and:

``` text
pinned to a selected CPU
```

Compare latency distributions.

### Experiment 7 --- False sharing

Construct two versions of queue metadata:

``` text
Version A:
read_index and write_index close together

Version B:
indexes separated onto different cache lines
```

Measure whether the change affects performance.

This experiment is optional and will only be performed after the queue
is correct.

### Experiment 8 --- Linux profiling

Use Linux performance tools to investigate:

-   CPU cycles
-   instructions
-   context switches
-   cache-related events where available
-   hot functions

The goal is to explain observed behavior rather than merely collect
numbers.

------------------------------------------------------------------------

## 11. Tools and Technologies

### Language

-   C++20

### Platform

-   Linux

### Core APIs

-   `std::thread`
-   `std::atomic`
-   mutexes
-   condition variables
-   Linux CPU affinity APIs where required
-   POSIX/Linux timing and system interfaces where appropriate

### Build

-   CMake
-   GCC or Clang

### Testing and debugging

-   GoogleTest or a lightweight custom test framework
-   AddressSanitizer
-   UndefinedBehaviorSanitizer
-   ThreadSanitizer where supported by the selected toolchain

### Performance

-   `perf`
-   high-resolution timing facilities
-   custom latency histogram collection

### Version control

-   Git

------------------------------------------------------------------------

## 12. What Is Explicitly Out of Scope?

To keep the project achievable in approximately two months, we will
**not** attempt to build:

-   a real exchange
-   real market connectivity
-   a complete TCP implementation
-   a distributed matching engine
-   NUMA-aware architecture as a required feature
-   a fully general-purpose MPMC lock-free queue
-   kernel modifications
-   FPGA acceleration
-   production-grade financial infrastructure
-   a complex graphical trading interface

These can be discussed as future extensions but are not required for the
course project.

------------------------------------------------------------------------

## 13. Expected OS Concepts

The project directly connects to OS topics including:

  OS / Systems Concept      Project Application
  ------------------------- ---------------------------------------
  Processes/threads         Producer and engine threads
  Thread synchronization    Mutex and condition-variable baseline
  Atomic operations         Lock-free queue
  Memory ordering           Producer/consumer synchronization
  CPU scheduling            Thread execution and contention
  CPU affinity              Controlled performance experiments
  Context switching         Performance analysis
  Memory/cache behavior     Ring-buffer layout and locality
  Race conditions           Concurrent queue correctness
  Linux system interfaces   Affinity, timing, profiling
  Performance measurement   Latency and throughput experiments

------------------------------------------------------------------------

## 14. Expected Deliverables

### Software

1.  Correct single-threaded matching engine
2.  Order-book implementation
3.  Threaded producer/consumer infrastructure
4.  Mutex-based baseline
5.  SPSC ring-buffer implementation
6.  Benchmarking framework
7.  Latency histogram/reporting
8.  Automated correctness/stress tests
9.  Linux profiling scripts/configuration
10. Documentation

### Report

The final report will contain:

-   system architecture
-   order-book design
-   concurrency design
-   synchronization design
-   experimental methodology
-   benchmark results
-   latency distributions
-   profiling results
-   explanation of bottlenecks
-   limitations
-   future improvements

------------------------------------------------------------------------

## 15. Expected Learning Outcomes

By the end of the project, the team should be able to explain and
demonstrate:

1.  How an order matching engine works.
2.  Why concurrent access to shared state requires synchronization.
3.  Why a single-threaded state owner can sometimes outperform a heavily
    locked design.
4.  How a ring buffer works.
5.  How atomic operations allow threads to coordinate without a
    traditional mutex.
6.  What acquire/release memory ordering provides.
7.  What race conditions look like in real software.
8.  How contention affects performance.
9.  Why tail latency matters.
10. How Linux tools can be used to investigate performance rather than
    guessing about it.
11. Why an optimization should be justified by measurement.

------------------------------------------------------------------------

## 16. Why This Is an OS Project

Although the application domain is an order book, the central
engineering questions are operating-system and systems questions.

The project intentionally uses a real workload to investigate:

``` text
threads
   ↓
synchronization
   ↓
atomic operations
   ↓
memory visibility
   ↓
CPU scheduling
   ↓
cache behavior
   ↓
Linux profiling
   ↓
latency
```

Instead of simulating an OS scheduler or implementing textbook
algorithms in isolation, the project allows us to observe how actual
Linux scheduling, synchronization, and hardware behavior affect a real
concurrent application.

------------------------------------------------------------------------

## 17. Team Structure

For a five-person team:

### Member 1 --- Matching Engine

-   order representation
-   price levels
-   matching rules
-   cancellation
-   correctness tests

### Member 2 --- Concurrent Queues

-   mutex queue
-   SPSC ring buffer
-   atomic indexes
-   concurrency stress tests

### Member 3 --- Benchmarking

-   workload generation
-   timing
-   throughput measurement
-   latency collection
-   percentile calculations

### Member 4 --- Linux Performance

-   `perf`
-   CPU affinity
-   context-switch analysis
-   cache/performance-counter experiments
-   profiling

### Member 5 --- Integration & Testing

-   integration framework
-   correctness testing
-   sanitizer setup
-   reproducible experiments
-   visualization/report infrastructure

All members should understand the complete architecture; ownership is
for implementation efficiency, not isolation.

------------------------------------------------------------------------

## 18. Success Criteria

The project will be considered successful if we can demonstrate:

1.  Correct matching behavior under extensive testing.
2.  Correct concurrent event ingestion.
3.  A working conventional baseline.
4.  A working low-overhead ring-buffer design.
5.  Reproducible performance benchmarks.
6.  Meaningful latency distributions rather than only average latency.
7.  At least one experimentally demonstrated performance effect.
8.  A technically defensible explanation for the observed results.

The most important result is not:

> "Our implementation is X% faster."

It is:

> "Under workload Y, design A behaves differently from design B because
> of mechanism Z, and our measurements support that explanation."

------------------------------------------------------------------------

## 19. Future Extensions

If the core project is completed early, possible extensions include:

-   MPSC queue
-   multiple matching partitions
-   batching
-   CPU affinity optimization
-   cache-line padding
-   custom memory pools
-   asynchronous network ingestion
-   persistent event logging
-   replaying recorded workloads
-   NUMA experiments on suitable hardware
-   network-based order ingestion

These will remain optional so that advanced topics do not compromise the
core project.

------------------------------------------------------------------------

## 20. Summary

We propose a **Linux-based low-latency event-processing system centered
around an order matching engine**.

The project starts with a simple and correct order book and
progressively introduces concurrency, synchronization, atomic
operations, ring buffers, Linux scheduling controls, and performance
analysis.

The emphasis is not on financial simulation. The order book is a
realistic, deterministic workload that gives us a concrete environment
in which to study operating systems, concurrency, and performance
engineering.

The final system will therefore be both:

-   coherent with an Operating Systems course, and
-   relevant to real systems/software engineering problems involving
    Linux, concurrency, backend infrastructure, and low-latency
    processing.
