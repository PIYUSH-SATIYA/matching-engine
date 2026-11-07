#pragma once

// Matching Engine (contract section 8).
//
//   Order  --process()-->  [matching decision]  --> Order Book changes
//                                               --> Engine Events / Trades
//
// * Single-threaded: only the engine thread calls process() and only it
//   touches the Order Book.
// * Template on Book and Sink so there is no virtual call on the hot path:
//     Book : OrderBook (V1) or OrderBookFast (V3)  - same interface
//     Sink : anything with onEvent(const EngineEvent&) / onTrade(const Trade&)
// * Matching rules: price-time priority; a trade executes at the RESTING
//   order's price; an incoming order sweeps as many levels as it can and
//   the unfilled remainder rests in the book.
// * One clock read per process() call. Every event/trade of that order
//   shares that timestamp (reading the clock per event would cost more than
//   some of the matching work itself).

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "order.hpp"

// ------------------------------------------------------------------ sinks
struct NullSink {
  void onEvent(const EngineEvent&) {}
  void onTrade(const Trade&) {}
};

// Cheap sink for tests/benchmarks: just counts. Also usable as a checksum
// (identical counts + traded quantity across implementations = same result).
struct CountingSink {
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t trade_events = 0;
  std::uint64_t partial_fills = 0;
  std::uint64_t full_fills = 0;
  std::uint64_t trades = 0;
  std::uint64_t traded_quantity = 0;
  std::uint64_t traded_notional = 0;  // sum(price * quantity), order-sensitive checksum

  void onEvent(const EngineEvent& e) {
    switch (e.type) {
      case EventType::OrderAccepted: ++accepted; break;
      case EventType::OrderRejected: ++rejected; break;
      case EventType::TradeExecuted: ++trade_events; break;
      case EventType::OrderPartiallyFilled: ++partial_fills; break;
      case EventType::OrderFullyFilled: ++full_fills; break;
    }
  }
  void onTrade(const Trade& t) {
    ++trades;
    traded_quantity += t.quantity;
    traded_notional += static_cast<std::uint64_t>(t.price) * t.quantity;
  }
};

// ----------------------------------------------------------------- engine
template <class Book, class Sink>
class MatchingEngine {
 public:
  MatchingEngine(Book& book, Sink& sink) : book_(book), sink_(sink) {}

  // Process ONE incoming order to completion.
  void process(const Order& incoming) {
    using Clock = std::chrono::steady_clock;
    const Timestamp now = Clock::now();

    Order in = incoming;

    // Reject before touching the book (book stays unchanged).
    if (in.quantity == 0 || !book_.validPrice(in.price) ||
        book_.contains(in.id)) {
      emit(EventType::OrderRejected, now, in.id, 0, in.quantity, in.price);
      return;
    }
    emit(EventType::OrderAccepted, now, in.id, 0, in.quantity, in.price);

    const Quantity original_quantity = in.quantity;

    if (in.side == Side::Buy) {
      while (in.quantity > 0 && book_.hasAsk() && book_.bestAsk() <= in.price) {
        Order* resting = book_.frontAtBestAsk();
        fill(in, resting->id, resting->price, resting->quantity,
             /*incoming_is_buy=*/true, now);
      }
    } else {
      while (in.quantity > 0 && book_.hasBid() && book_.bestBid() >= in.price) {
        Order* resting = book_.frontAtBestBid();
        fill(in, resting->id, resting->price, resting->quantity,
             /*incoming_is_buy=*/false, now);
      }
    }

    if (in.quantity == 0) {
      emit(EventType::OrderFullyFilled, now, in.id, 0, original_quantity, in.price);
      return;
    }
    if (in.quantity < original_quantity) {  // traded some, rest the remainder
      emit(EventType::OrderPartiallyFilled, now, in.id, 0,
           original_quantity - in.quantity, in.price);
    }
    if (!book_.add(in)) {  // e.g. order pool exhausted
      emit(EventType::OrderRejected, now, in.id, 0, in.quantity, in.price);
      ++rejected_remainders_;
    }
  }

  std::uint64_t tradeCount() const { return next_trade_id_ - 1; }
  std::uint64_t rejectedRemainders() const { return rejected_remainders_; }

 private:
  // Execute one match between `in` and the order at the front of the best
  // opposite level. `resting_*` are copied BEFORE reduce() (the node may be freed).
  void fill(Order& in, OrderId resting_id, Price resting_price,
            Quantity resting_qty, bool incoming_is_buy, Timestamp now) {
    const Quantity q = std::min(in.quantity, resting_qty);
    const TradeId tid = next_trade_id_++;

    Trade t{};
    t.id = tid;
    t.buy_order_id = incoming_is_buy ? in.id : resting_id;
    t.sell_order_id = incoming_is_buy ? resting_id : in.id;
    t.price = resting_price;  // trade at the RESTING order's price
    t.quantity = q;
    t.execution_time = now;
    sink_.onTrade(t);
    emit(EventType::TradeExecuted, now, in.id, tid, q, resting_price);

    in.quantity -= q;
    book_.reduce(resting_id, q);  // removes it (and empty level) if q == resting_qty
    emit(q == resting_qty ? EventType::OrderFullyFilled
                          : EventType::OrderPartiallyFilled,
         now, resting_id, tid, q, resting_price);
  }

  void emit(EventType type, Timestamp ts, OrderId order_id, TradeId trade_id,
            Quantity qty, Price price) {
    EngineEvent e{};
    e.type = type;
    e.timestamp = ts;
    e.order_id = order_id;
    e.trade_id = trade_id;
    e.quantity = qty;
    e.price = price;
    sink_.onEvent(e);
  }

  Book& book_;
  Sink& sink_;
  TradeId next_trade_id_ = 1;
  std::uint64_t rejected_remainders_ = 0;
};
