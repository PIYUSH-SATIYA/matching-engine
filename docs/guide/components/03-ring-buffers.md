# 03 — Ring buffers: where `Order` waits between threads

The network thread produces `Order` objects. The engine thread consumes them.
The queue is the ownership-transfer boundary.

```text
network thread                         engine thread
producer                               consumer
    |                                      ^
    | push(Order)                          | pop() -> Order
    +------------ ring buffer -------------+
```

The application selects the implementation in `src/main.cpp`:

```cpp
if (opt.queue == "spsc")
    return runWithBook<SpscRingBuffer<16384>>(opt);
return runWithBook<RingBuffer>(opt);
```

**Source:** `src/main.cpp:546-552`.

## Common ring-buffer idea

```text
head = next item to consume
tail = next slot to produce into
```

As positions advance, they wrap to the beginning. A ring reuses a fixed array
instead of allocating a new queue node for every `Order`.

## Mutex ring buffer

Storage and state:

```cpp
std::array<Order, CAPACITY> buffer{};
std::size_t tail = 0;
std::size_t head = 0;
std::size_t count = 0;
std::mutex mtx;
std::condition_variable not_empty;
std::condition_variable not_full;
```

**Source:** `include/ring_buffer.hpp:16-31`.

Here `count` is authoritative:

```text
count == 0          queue empty
count == CAPACITY   queue full
0 < count < cap     queue contains count orders
```

### Producer path: `push`

```cpp
std::unique_lock<std::mutex> lock(mtx);
if (count == CAPACITY) {
    waits_full_++;
    not_full.wait(lock, [this] { return count < CAPACITY; });
}
buffer[tail] = ord;
tail = (tail + 1) % CAPACITY;
++count;
lock.unlock();
not_empty.notify_one();
```

**Source:** `src/ring_buffer.cpp:6-23`.

In plain English: lock; sleep if full; copy the `Order` into `tail`; advance
`tail`; increment `count`; unlock; wake a consumer.

### Consumer path: `pop`

```cpp
std::unique_lock<std::mutex> lock(mtx);
if (count == 0) {
    waits_empty_++;
    not_empty.wait(lock, [this] { return count > 0; });
}
Order ord = buffer[head];
head = (head + 1) % CAPACITY;
--count;
lock.unlock();
not_full.notify_one();
return ord;
```

**Source:** `src/ring_buffer.cpp:25-45`.

The order is copied out before the consumer advances `head`. The queue does not
remove or alter any book order; it only returns the next network-produced order.

## SPSC ring buffer

The SPSC queue has no shared `count`. It uses monotonically increasing counters:

```cpp
alignas(64) std::atomic<std::size_t> head_{0};
alignas(64) std::atomic<std::size_t> tail_{0};
Order buf_[Capacity]{};
```

**Source:** `include/spsc_ring_buffer.hpp:145-153`.

Ownership is the key:

```text
consumer writes head_
producer writes tail_
producer reads head_
consumer reads tail
```

Because one thread owns each write index, neither side needs a mutex or
compare-and-swap loop. Capacity is a power of two, so the slot is
`counter & (Capacity - 1)`.

**Source:** `include/spsc_ring_buffer.hpp:45-49`, `:82`, and `:100`.

### SPSC full test and push

```cpp
const std::size_t t = tail_.load(relaxed);
if (t - cached_head_ >= Capacity) {
    cached_head_ = head_.load(acquire);
    if (t - cached_head_ >= Capacity) return false;
}
buf_[t & kMask] = o;
tail_.store(t + 1, std::memory_order_release);
```

**Source:** `include/spsc_ring_buffer.hpp:76-84`.

The producer is full when `tail - head >= Capacity`. The order is written first;
the release store publishes that the slot is ready.

### SPSC empty test and pop

```cpp
const std::size_t h = head_.load(relaxed);
if (h == cached_tail_) {
    cached_tail_ = tail_.load(acquire);
    if (h == cached_tail_) return false;
}
out = buf_[h & kMask];
head_.store(h + 1, std::memory_order_release);
```

**Source:** `include/spsc_ring_buffer.hpp:94-102`.

The consumer is empty when `head == tail`. The acquire load ensures it sees the
producer's completed slot write.

### SPSC waiting

```cpp
if (try_push(o)) return;
g_full_waits++;
for (unsigned spins = 0; !try_push(o); ++spins)
    wait_hint(spins);
```

**Source:** `include/spsc_ring_buffer.hpp:87-91`.

`wait_hint()` uses a CPU pause for short waits and periodically yields after
4096 spins. SPSC trades sleeping and wake-up latency for CPU consumption.

## Direct answer to the insertion question

```text
network layer creates the Order
network layer copies it into the queue
matching engine copies it out of the queue
matching engine processes it
```

The network layer does **not** insert it into the order book. Only the matching
engine calls `book.add()`.
