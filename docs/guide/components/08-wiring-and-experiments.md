# 08 — Wiring and experiment combinations

This file answers the experimental-design question directly.

## How selected components are wired

`runApp<Queue, Book>()` constructs the selected queue and book:

```cpp
auto queue = std::make_unique<Queue>();
auto book = std::make_unique<Book>();
```

**Source:** `src/main.cpp:175-178`.

The network callback is bound to that queue:

```cpp
auto push_fn = [](void* ctx, const Order& o) {
    static_cast<Queue*>(ctx)->push(o);
};
```

**Source:** `src/main.cpp:180-182`.

The `--net` choice selects the server, but both receive the same callback and
queue context:

```cpp
server = (opt.net == "uring")
    ? makeUringServer(opt.port, push_fn, queue.get(), ...)
    : makeEpollServer(opt.port, push_fn, queue.get(), ...);
```

**Source:** `src/main.cpp:184-189`.

The engine thread receives the selected book:

```cpp
MatchingEngine<Book, CountingSink> engine(*book, counting);
```

**Source:** `src/main.cpp:302-309`.

Book dispatch:

```cpp
if (opt.book == "v1")
    runApp<Queue, OrderBook>(opt);
else if (opt.book == "fast-heap")
    runApp<Queue, OrderBookFast<..., true>>(opt);
else
    runApp<Queue, OrderBookFast<...>>(opt); // fast-slab
```

**Source:** `src/main.cpp:446-451`.

Queue dispatch:

```cpp
if (opt.queue == "spsc")
    return runWithBook<SpscRingBuffer<16384>>(opt);
return runWithBook<RingBuffer>(opt);
```

**Source:** `src/main.cpp:546-552`.

## Is your “fix one component, permute the others” understanding correct?

Yes, with one wording correction: experiments usually **hold one component
choice fixed while varying another**, rather than blindly running every possible
combination.

To compare network backends:

```text
choose one queue, one book, one scheduling mode
run epoll
run io_uring
compare latency, throughput, and counters
```

To compare queues:

```text
choose one network backend, one book, one scheduling mode
run mutex queue
run SPSC queue
compare latency, throughput, and counters
```

To compare books:

```text
choose one queue, one network backend, one scheduling mode
run V1
run V3 slab
run V3 heap when allocator control is useful
compare semantics and performance
```

This is a **single-variable comparison**. Changing epoll, queue, book, and
scheduler at the same time does not identify a cause.

## Complete theoretical product

```text
queue:      mutex | spsc                 (2)
book:       v1 | fast-slab | fast-heap   (3)
network:    epoll | uring                 (2)
scheduler:  default | pin | rt            (3)
```

There are `2 × 3 × 2 × 3 = 36` theoretical combinations. Not every one is a
headline experiment: `rt` needs permission, `uring` needs liburing, and focused
experiments deliberately hold dimensions constant.

## Actual comparison families

| Question | Fixed choices | Changed choices |
|---|---|---|
| queue tail effect | V3 slab, epoll, selected scheduling | mutex vs SPSC |
| I/O API effect | selected queue, V3 slab, selected scheduling | epoll vs io_uring |
| engine affinity | selected queue, V3 slab, epoll | default vs pin |
| allocator effect | V3 structure, queue/network/scheduling | slab vs heap |
| book structure | queue/network/scheduling | V1 vs V3 |
| overload | selected complete pipeline | offered rate |

## “Fix epoll” means two different things

### Meaning A — epoll is the controlled constant

This is correct for a queue experiment:

```text
epoll + V3 slab + default + mutex
epoll + V3 slab + default + SPSC
```

Only queue type changed.

### Meaning B — every combination containing epoll is mandatory

Not necessarily. That can create many runs without answering a new question. State
the question first, then choose the smallest controlled comparison that answers
it.

The rule is:

> Every claim must identify which dimensions were held fixed and which dimension
> was varied.
