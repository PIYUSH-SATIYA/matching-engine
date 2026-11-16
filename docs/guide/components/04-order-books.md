# 04 — Order books: where resting orders live

The incoming order is held in the matching engine's local variable. The book
already contains **resting opposite-side orders**. The engine searches those
resting orders directly, creates trades, and inserts only the incoming remainder.

## The book is storage, not the matcher

```cpp
bool hasBid() const;
bool hasAsk() const;
Price bestBid() const;
Price bestAsk() const;
Order* frontAtBestBid();
Order* frontAtBestAsk();
bool add(const Order& order);
bool reduce(OrderId id, Quantity q);
bool cancel(OrderId id);
```

**Source:** `include/order_book.hpp:25-62` and
`include/order_book_fast.hpp:86-157`.

There is no `book.match()` function. The matching engine calls these operations
in the correct price-time sequence.

## V1: map of price levels containing lists of orders

```cpp
using Level  = std::list<Order>;
using BidMap = std::map<Price, Level, std::greater<Price>>;
using AskMap = std::map<Price, Level>;

BidMap bids_;
AskMap asks_;
std::unordered_map<OrderId, Locator> index_;
```

**Source:** `include/order_book.hpp:64-79`.

```text
bids_ (descending map)                 asks_ (ascending map)
102 -> [B7, B9]                         103 -> [S2, S8]
101 -> [B4]                             104 -> [S5]
100 -> [B1, B3]                         105 -> [S6]
^ best bid                              ^ best ask
```

Within each list, the front is oldest and the back is newest.

### Finding the best resting order

```cpp
Order* OrderBook::frontAtBestBid() {
    if (bids_.empty()) return nullptr;
    return &bids_.begin()->second.front();
}

Order* OrderBook::frontAtBestAsk() {
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second.front();
}
```

**Source:** `src/order_book.cpp:19-27`.

The bid comparator makes `begin()` the highest price. The normal ask comparator
makes `begin()` the lowest price. `.front()` applies time priority.

### Adding a resting V1 order

```cpp
if (order.side == Side::Buy) {
    Level& level = bids_[order.price];
    level.push_back(order);
    it = std::prev(level.end());
} else {
    Level& level = asks_[order.price];
    level.push_back(order);
    it = std::prev(level.end());
}
index_.emplace(order.id, Locator{order.side, order.price, it});
```

**Source:** `src/order_book.cpp:29-45`.

This is where a non-crossing order or an unfilled remainder becomes a resting
V1 order: it is copied to the back of the list for its price.

### Partial fill and removal

```cpp
if (q == resting.quantity) {
    eraseOrder(found->second);
    index_.erase(found);
} else {
    resting.quantity -= q;
}
```

**Source:** `src/order_book.cpp:47-61`.

Partial reduction changes quantity but does not move the list node. Full reduction
unlinks it and removes an empty price level.

## V3: direct price arrays and intrusive lists

V3 declares a fixed price universe:

```cpp
std::vector<Level> bids_(NumLevels);
std::vector<Level> asks_(NumLevels);
```

Each `Level` is an intrusive doubly linked list:

```cpp
struct Level {
    Node* head = nullptr; // oldest
    Node* tail = nullptr; // newest
};
```

**Source:** `include/order_book_fast.hpp:197-200`.

A price maps directly to an array slot:

```cpp
idx = static_cast<std::size_t>(order.price - min_price_);
Level& lvl = (is_buy ? bids_ : asks_)[idx];
```

**Source:** `include/order_book_fast.hpp:117-120`.

There is no `std::map` tree in V3.

### V3 node layout

```cpp
struct alignas(64) Node {
    Order order;
    Node* prev;
    Node* next;
};
static_assert(sizeof(Node) == 64);
```

**Source:** `include/order_book_fast.hpp:186-192`.

### Adding a resting V3 order

```cpp
Node* n = slab_->allocate();
if (n == nullptr) return false;
new (n) Node{order, nullptr, nullptr};

n->prev = lvl.tail;
if (lvl.tail) lvl.tail->next = n;
else         lvl.head = n;
lvl.tail = n;
```

**Source:** `include/order_book_fast.hpp:109-123`.

This is the exact place where a valid incoming remainder becomes a V3 resting
order. It is not inserted before the matching loop.

### V3 best-price metadata

When a level changes from empty to non-empty:

```cpp
if (is_buy) {
    bid_map_.set(idx);
    if (best_bid_ == kNone || idx > best_bid_)
        best_bid_ = idx;
} else {
    ask_map_.set(idx);
    if (best_ask_ == kNone || idx < best_ask_)
        best_ask_ = idx;
}
```

**Source:** `include/order_book_fast.hpp:125-134`.

When a level becomes empty, V3 clears its bitmap bit and finds the next set bit
with `prevSet()` or `nextSet()`.

### V3 ID lookup

```cpp
static constexpr std::size_t kHashSize = nextPow2(MaxOrders * 2);
HashSlot hash_[kHashSize];
```

**Source:** `include/order_book_fast.hpp:253-276`.

V3 stores `OrderId -> Node*` in a pre-sized open-addressing table. Deletion uses
backward shifting rather than tombstones.

## Does the incoming order enter the book first?

No. The engine first keeps it local and looks up the opposite side:

```cpp
Order in = incoming;
if (in.side == Side::Buy) {
    while (in.quantity > 0 && book_.hasAsk() &&
           book_.bestAsk() <= in.price) {
        Order* resting = book_.frontAtBestAsk();
        fill(in, resting->id, resting->price, resting->quantity, true, now);
    }
}
```

**Source:** `include/matching_engine.hpp:66-95`.

Only after that loop:

```cpp
if (in.quantity == 0) return; // fully filled; never added
if (in.quantity < original_quantity)
    emit(OrderPartiallyFilled, ...);
if (!book_.add(in))
    emit(OrderRejected, ...);
```

**Source:** `include/matching_engine.hpp:97-109`.

Therefore:

```text
incoming order -> local variable `in`
                 -> inspect opposite-side book
                 -> trade and shrink `in.quantity`
                 -> book.add(in) only if quantity remains
```

The opposite-side orders are already inside the book. The incoming order is not.
