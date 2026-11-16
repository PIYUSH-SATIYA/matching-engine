# 07 — Sinks, events, and measurements

The matching engine produces semantic results and observability results.

## Trade versus event

```cpp
struct Trade {
  TradeId id;
  OrderId buy_order_id;
  OrderId sell_order_id;
  Price price;
  Quantity quantity;
  Timestamp execution_time;
};
```

**Source:** `include/order.hpp:42-52`.

`EngineEvent` has types `OrderAccepted`, `OrderRejected`, `TradeExecuted`,
`OrderPartiallyFilled`, and `OrderFullyFilled`.

**Source:** `include/order.hpp:16-22`.

One incoming order can produce multiple trades and multiple events.

## Counting sink

```cpp
void onTrade(const Trade& t) {
    ++trades;
    traded_quantity += t.quantity;
    traded_notional += price * quantity;
}
```

**Source:** `include/matching_engine.hpp:34-57`.

The benchmark uses trade count, traded quantity, and traded notional as a semantic
checksum. V1 and V3 must agree before their performance can be compared.

## Optional event path

With `--log`, `main.cpp` creates an `EventManager`, registers a listener, and uses
a tee sink:

```cpp
events = std::make_unique<EventManager>();
events->registerListener([log](const EngineEvent& e) {
    log->onEvent(e);
});
TeeSink<CountingSink, EventManagerSink> tee(counting, ems);
MatchingEngine<Book, decltype(tee)> engine(book, tee);
```

**Source:** `src/main.cpp:202-217` and `:302-309`.

The event manager broadcasts to listeners:

```cpp
for (const auto& listener : listeners_)
    listener(event);
```

**Source:** `src/event_manager.hpp:24-41`.

Measured runs normally use `CountingSink` alone so logging and file writes do not
become part of the latency path.

## Timing

The engine thread does:

```cpp
Order o = queue->pop();
auto t0 = o.submit_time;
engine.process(o);
auto t1 = steady_clock::now();
```

**Source:** `src/main.cpp:246-276`.

Then it computes:

```cpp
arr = o.enqueue_time - t0;
svc = t1 - o.enqueue_time;
```

**Source:** `src/main.cpp:277-290`.

So arrival includes client/TCP/network-thread delay, while service includes queue
wait plus matching/book work.
