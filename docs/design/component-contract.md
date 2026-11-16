# Phase 1 — Component Contract

## Purpose

This document defines the interfaces and responsibilities between the components of the low-latency order matching engine.

The goal is to make the components independently implementable while keeping the integration points fixed.

**Important:** This document defines external behavior and ownership. It does **not** prescribe the internal data structures or algorithms used inside each component.

---

# 1. System Architecture

```text
Clients
   |
   v
Network Layer
   |
   | Order
   v
Ring Buffer
   |
   | Order
   v
Matching Engine
   | \
   |  \
   |   +--------------------+
   |                        |
   v                        v
Order Book              Engine Events
   |                        |
   v                        v
Slab Allocator         Event Manager
                            |
                       +----+----+
                       |         |
                       v         v
                   Benchmark   Logger
```

## Core ownership rule

The Matching Engine is single-threaded.

Only the Matching Engine thread may mutate the Order Book.

The Network Layer, Ring Buffer, Event Manager, Benchmark, and Logger must not directly modify Order Book state.

---

# 2. Shared Domain Types

All components use the shared types defined in:

```text
include/order.hpp
```

The shared types are:

- `Order`
- `Trade`
- `EngineEvent`
- `OrderId`
- `TradeId`
- `Price`
- `Quantity`
- `Timestamp`
- `Side`
- `EventType`

These are the common language between components.

Do not create alternative versions of these structures inside individual components.

---

# 3. Order

## Purpose

Represents an order entering the matching system.

## Created by

Network Layer.

## Transferred through

Network Layer → Ring Buffer → Matching Engine.

## Consumed by

Matching Engine.

## Important ownership rule

Before entering the Matching Engine, the order is transport data.

Once consumed by the Matching Engine, the Matching Engine owns the order's mutable state.

The Network Layer must not modify an order after submitting it to the Ring Buffer.

The Ring Buffer is responsible only for safely transferring the order.

## Required information

An Order contains:

- Order ID
- Buy/Sell side
- Price
- Quantity
- Submission timestamp

The submission timestamp must represent when the order entered the system, not when it reached the Matching Engine.

---

# 4. Trade

## Purpose

Represents an actual execution produced by the Matching Engine.

One incoming Order may produce:

- zero trades
- one trade
- multiple trades

## Created by

Matching Engine.

## Consumed by

Event Manager / event-generation layer.

## Required information

A Trade contains:

- Trade ID
- Buy Order ID
- Sell Order ID
- Execution price
- Executed quantity
- Execution timestamp

A Trade is a result of matching, not an input to the Matching Engine.

---

# 5. Engine Event

## Purpose

Represents something meaningful that happened inside the Matching Engine.

Events are passed to the Event Manager.

Examples:

- Order accepted
- Order rejected
- Trade executed
- Order partially filled
- Order fully filled

## Created by

Matching Engine.

## Consumed by

Event Manager.

The Event Manager does not decide whether an order should match. It only receives and distributes events.

---

# 6. Network Layer

## Responsibility

Accept client requests and convert them into valid `Order` objects.

## Input

External client request / network data.

## Output

A valid `Order`.

The resulting Order is submitted to the Ring Buffer.

## Network Layer owns

- Client connections
- Network protocol
- Parsing
- Input validation
- Connection lifecycle

## Network Layer does NOT own

- Matching logic
- Order Book state
- Trade generation
- Benchmark calculation
- Logging

## Boundary

```text
External Client
      |
      v
Network Layer
      |
      | Order
      v
Ring Buffer
```

The Network Layer should not know how the Matching Engine stores or matches orders.

---

# 7. Ring Buffer

## Responsibility

Safely transfer Orders from the Network Layer thread to the Matching Engine thread.

## Producer

Network Layer.

## Consumer

Matching Engine.

## Input

`Order`

## Output

`Order`

## Required operations

The conceptual interface consists of:

- Push an Order
- Pop an Order
- Detect full/empty conditions

The exact class/function names are implementation details.

## Two implementations

Phase 1 requires two interchangeable implementations:

1. Mutex-based queue
2. SPSC lock-free queue

Both implementations must provide the same external behavior.

The Matching Engine must not need to know which implementation is currently being used.

## Important concurrency rule

The SPSC implementation assumes exactly:

- one producer
- one consumer

Do not silently extend it to multiple producers.

## Ownership

The Ring Buffer owns the storage used for queued Orders.

Once an Order is successfully popped, the Matching Engine owns that Order's processing state.

## Ring Buffer does NOT

- match orders
- inspect Buy/Sell semantics
- modify Order Book state
- generate trades
- perform logging
- calculate latency

---

# 8. Matching Engine

## Responsibility

Process Orders sequentially and perform matching according to the project's matching rules.

## Input

One `Order` at a time from the Ring Buffer.

## Uses

Order Book.

## Output

Engine Events / Trades resulting from processing the Order.

## Core rule

The Matching Engine is single-threaded.

Only its thread accesses mutable Order Book state.

## Matching Engine does NOT

- receive network connections
- implement the Ring Buffer
- write log files
- calculate benchmark statistics
- directly manage client connections

Its responsibility is:

```text
Order
  |
  v
Matching decision
  |
  +--> Order Book changes
  |
  +--> Engine Events
```

---

# 9. Order Book

## Responsibility

Maintain the current state of resting orders and provide the state required by the Matching Engine to perform matching.

## Owner

Matching Engine thread.

## Called by

Matching Engine.

## Provides conceptually

The Matching Engine must be able to:

- inspect the best available prices
- access orders at relevant price levels
- add resting orders
- reduce/remove filled orders
- maintain price-time priority

The exact internal data structures are intentionally left to the Order Book owner.

## Critical rule

No other component directly accesses or modifies the Order Book.

## Order Book does NOT

- receive network requests
- manage threads
- synchronize with the Network Layer
- write logs
- calculate benchmark metrics

---

# 10. Slab Allocator

## Responsibility

Provide and reclaim memory for objects used by the Order Book.

## Called by

Order Book / Matching Engine-owned data structures.

## Input

Allocation or release request.

## Output

Available memory/object storage.

## Ownership

The allocator owns its memory pool.

The Order Book owns objects after successfully acquiring storage from the allocator.

When an object is no longer needed, the Order Book returns the storage to the allocator.

## Slab Allocator does NOT

- understand Buy/Sell logic
- perform matching
- maintain price levels
- generate events
- know about network clients

Its only concern is memory management.

---

# 11. Event Manager

## Responsibility

Receive events generated by the Matching Engine and distribute them to interested consumers.

## Input

`EngineEvent`

## Consumers

- Benchmark
- Logger

## Flow

```text
Matching Engine
      |
      | EngineEvent
      v
Event Manager
      |
      +------> Benchmark
      |
      +------> Logger
```

## Event Manager does NOT

- perform matching
- modify the Order Book
- decide whether an order is valid
- calculate matching results
- implement benchmark algorithms
- implement log storage

Its responsibility is event distribution.

---

# 12. Benchmark Service

## Responsibility

Measure system performance using information supplied by the system's events/timestamps.

## Input

Relevant Engine Events and timestamps.

## Produces

Performance measurements such as:

- latency samples
- throughput
- number of orders processed
- number of trades generated
- p50 latency
- p95 latency
- p99 latency
- p99.9 latency
- maximum latency

## Important rule

Benchmarking and logging are separate concerns.

Benchmark code should avoid unnecessary work that significantly changes the performance characteristics being measured.

## Benchmark does NOT

- modify the Matching Engine
- modify the Order Book
- control the Ring Buffer
- decide matching behavior

---

# 13. Logger

## Responsibility

Record useful system events for debugging, analysis, and demonstration.

## Input

Engine Events.

## Produces

Log records, such as:

- order accepted
- order rejected
- trade executed
- order partially filled
- order fully filled
- errors or important system state

## Logger does NOT

- perform matching
- modify orders in the Order Book
- control synchronization
- calculate benchmark statistics

---

# 14. Component Ownership Summary

| Component | Receives | Produces | Main Owner |
|---|---|---|---|
| Network Layer | Client data | Order | Network owner |
| Ring Buffer | Order | Order | Ring Buffer owner |
| Matching Engine | Order | Engine Events / Trades | Matching owner |
| Order Book | Matching operations | Book state | Matching owner |
| Slab Allocator | Allocation/release requests | Memory/object storage | Allocator owner |
| Event Manager | Engine Events | Distributed events | Event owner |
| Benchmark | Events/timestamps | Metrics | Benchmark owner |
| Logger | Events | Log records | Logger owner |

---

# 15. What Must Stay Stable

The following are integration contracts.

Do not change them casually after other components start depending on them:

1. Meaning of `Order`
2. Meaning of `Trade`
3. Meaning of `EngineEvent`
4. Who produces and consumes Orders
5. Who owns the Order Book
6. Who produces Engine Events
7. Event Manager's role
8. Ring Buffer producer/consumer relationship

If a contract needs to change, discuss it with the team before changing it.

---

# 16. What Is Deliberately NOT Specified

This document does NOT prescribe:

- Order Book data structures
- Ring Buffer implementation
- Slab allocator implementation
- Network implementation details
- Event Manager internal design
- Benchmark storage strategy
- Logger storage format
- Thread implementation details
- File layout beyond the existing project structure

Each component owner is free to choose appropriate internal structures as long as the external contract is respected.

---

# 17. Integration Principle

A component should depend on the **contract**, not on another component's internal implementation.

For example:

```text
Network Layer
     |
     | Order
     v
Ring Buffer
```

The Network owner does not need to know whether the Ring Buffer internally uses:

- mutexes
- atomics
- arrays
- another valid implementation

Similarly:

```text
Matching Engine
     |
     v
Order Book
```

The Matching Engine should depend on the Order Book's required behavior, not its internal data structures.

---

# 18. Phase 1 End-to-End Flow

The complete Phase 1 path is:

```text
Client
  |
  v
Network Layer
  |
  | Order
  v
Ring Buffer
  |
  | Order
  v
Matching Engine
  |
  +------> Order Book
  |
  | Engine Events
  v
Event Manager
  |
  +------> Benchmark
  |
  +------> Logger
```

The system is considered integrated when an Order can travel through this entire path correctly.

---

# 19. Team Rule

Before implementing a component:

1. Read this contract.
2. Confirm what your component receives.
3. Confirm what your component produces.
4. Confirm who owns the data at each boundary.
5. Do not depend on another component's internal data structures.
6. If you believe the contract is insufficient or incorrect, raise it before changing your implementation.

The goal is not for everyone to build independently and connect everything at the end.

The goal is for everyone to build independently **against shared contracts** so that integration is predictable.
