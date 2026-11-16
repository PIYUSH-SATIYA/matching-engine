# 01 — Order and wire format

This component has one job: represent an order and move its fields across TCP.
It does not match anything.

## The in-memory `Order`

```cpp
struct Order {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
  Timestamp submit_time;
  Timestamp enqueue_time{};
};
```

**Source:** `include/order.hpp:24-40`.

| Field | Written by | Used by |
|---|---|---|
| `id` | wire parser | duplicate-ID lookup, trade/event identity |
| `side` | wire parser | buy loop or sell loop |
| `price` | wire parser | crossing test and book level |
| `quantity` | wire parser; local engine copy shrinks | fill amount and resting remainder |
| `submit_time` | client timestamp converted by parser | total/arrival latency |
| `enqueue_time` | network thread | service-latency boundary |

## The 40-byte wire message

```cpp
struct Msg {
    uint64_t id;
    uint8_t  side;        // 0 = Buy, 1 = Sell
    uint8_t  pad[7];
    int64_t  price;
    uint64_t quantity;
    uint64_t client_ts_ns;
};
```

**Source:** `include/wire.hpp:19-32`.

The client fills it in `tools/load_client.cpp`:

```cpp
m.id = sent + i + 1;
m.side = static_cast<std::uint8_t>(side_dist(rng));
m.price = static_cast<std::int64_t>(std::round(p));
m.quantity = qty_dist(rng);
m.client_ts_ns = ts;
```

**Source:** `tools/load_client.cpp:116-126`.

The client then sends all bytes, retrying short writes until the entire batch is
transmitted. **Source:** `tools/load_client.cpp:128-139`.

## Parser decision tree

`wire::parse()` consumes complete 40-byte records and preserves an incomplete
tail for the next socket read:

```cpp
while (used - offset >= kMsgSize) {
    copy 40 bytes into Msg m;
    offset += 40;
    if (!valid(m)) {
        ++invalid;
        continue;
    }
    on_msg(m);
}
move remaining partial bytes to the front;
return remaining;
```

**Source:** `include/wire.hpp:48-70`.

The wire-level validity test is deliberately small:

```cpp
return m.id != 0 && m.side <= 1 && m.quantity > 0;
```

**Source:** `include/wire.hpp:34-36`.

An out-of-range V3 price is not a wire error. The parser does not know the
book's configured price window; the matching engine checks it later.

## Conversion and timestamp boundary

Both network backends eventually execute this same logical sequence:

```cpp
Order o = wire::toOrder(m);
const auto now = steady_clock::now();
o.enqueue_time = now;
push_(ctx_, o);
```

**Source:** `src/network_epoll.cpp:163-177` and
`src/network_io_uring.cpp:204-220`.

This creates the two timing regions:

```text
arrival = enqueue_time - submit_time
service = engine_completion - enqueue_time
total   = engine_completion - submit_time
```
