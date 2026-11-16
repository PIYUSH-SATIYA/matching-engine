# 06 — Matching engine: the component that actually decides

The matching engine is the only component that interprets an order against book
state. It is templated over the book and sink:

```cpp
template <class Book, class Sink>
class MatchingEngine {
    Book& book_;
    Sink& sink_;
};
```

**Source:** `include/matching_engine.hpp:60-64`.

The same algorithm therefore runs with V1, V3 slab, or V3 heap.

## Early validation

```cpp
Order in = incoming;
if (in.quantity == 0 || !book_.validPrice(in.price) ||
    book_.contains(in.id)) {
    emit(EventType::OrderRejected, ...);
    return;
}
emit(EventType::OrderAccepted, ...);
```

**Source:** `include/matching_engine.hpp:66-79`.

The order has not been added to the book here. It is still the local variable `in`.

## Buy matching loop

```cpp
if (in.side == Side::Buy) {
    while (in.quantity > 0 && book_.hasAsk() &&
           book_.bestAsk() <= in.price) {
        Order* resting = book_.frontAtBestAsk();
        fill(in, resting->id, resting->price,
             resting->quantity, true, now);
    }
}
```

**Source:** `include/matching_engine.hpp:81-88`.

In English: while the incoming buy still wants quantity, a sell exists, and the
cheapest sell is affordable, select the oldest order at that price and execute a
fill.

## Sell matching loop

```cpp
else {
    while (in.quantity > 0 && book_.hasBid() &&
           book_.bestBid() >= in.price) {
        Order* resting = book_.frontAtBestBid();
        fill(in, resting->id, resting->price,
             resting->quantity, false, now);
    }
}
```

**Source:** `include/matching_engine.hpp:89-95`.

The sell loop is the mirror: choose the highest affordable bid, then the oldest
order at that price.

## One fill

```cpp
const Quantity q = std::min(in.quantity, resting_qty);
Trade t{};
t.id = next_trade_id_++;
t.buy_order_id = incoming_is_buy ? in.id : resting_id;
t.sell_order_id = incoming_is_buy ? resting_id : in.id;
t.price = resting_price;
t.quantity = q;
t.execution_time = now;
```

**Source:** `include/matching_engine.hpp:115-129`.

Then:

```cpp
sink_.onTrade(t);
emit(TradeExecuted, ...);
in.quantity -= q;
book_.reduce(resting_id, q);
emit(q == resting_qty ? OrderFullyFilled
                      : OrderPartiallyFilled, ...);
```

**Source:** `include/matching_engine.hpp:129-136`.

The resting ID, price, and quantity are copied before `reduce()`, because a full
reduction may destroy the resting node.

## Terminal incoming-order logic

```cpp
if (in.quantity == 0) {
    emit(OrderFullyFilled, ...);
    return;
}
if (in.quantity < original_quantity)
    emit(OrderPartiallyFilled, ...);
if (!book_.add(in)) {
    emit(OrderRejected, ...);
    ++rejected_remainders_;
}
```

**Source:** `include/matching_engine.hpp:97-109`.

Exact outcomes:

```text
incoming fully matched        -> never enters book
incoming partially matched    -> only remaining quantity enters book
incoming never matched        -> full incoming quantity enters book
incoming remainder cannot fit  -> remainder is rejected
```
