// Tests for the Matching Engine on BOTH order books (V1 and V3).

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "matching_engine.hpp"
#include "order_book.hpp"
#include "order_book_fast.hpp"

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

struct RecordingSink {
  std::vector<EngineEvent> events;
  std::vector<Trade> trades;
  void onEvent(const EngineEvent& e) { events.push_back(e); }
  void onTrade(const Trade& t) { trades.push_back(t); }
};

static Order mk(OrderId id, Side s, Price p, Quantity q) {
  Order o{};
  o.id = id; o.side = s; o.price = p; o.quantity = q;
  return o;
}

template <class Book>
static void scenarioTests(const char* name) {
  {  // exact match, trade at resting price, correct buyer/seller ids
    Book b; RecordingSink s; MatchingEngine<Book, RecordingSink> e(b, s);
    e.process(mk(1, Side::Buy, 100, 10));
    e.process(mk(2, Side::Sell, 60, 10));
    CHECK(s.trades.size() == 1);
    CHECK(s.trades[0].buy_order_id == 1 && s.trades[0].sell_order_id == 2);
    CHECK(s.trades[0].price == 100 && s.trades[0].quantity == 10);
    CHECK(b.orderCount() == 0);
    // events: Accepted(1), Accepted(2), TradeExecuted, FullyFilled(resting 1), FullyFilled(2)
    CHECK(s.events.size() == 5);
    CHECK(s.events[0].type == EventType::OrderAccepted && s.events[0].order_id == 1);
    CHECK(s.events[2].type == EventType::TradeExecuted);
    CHECK(s.events[3].type == EventType::OrderFullyFilled && s.events[3].order_id == 1);
    CHECK(s.events[4].type == EventType::OrderFullyFilled && s.events[4].order_id == 2);
  }
  {  // user's case: B1@100, S1@60, S2@50, B2@102 -> trades at 100 and 50
    Book b; RecordingSink s; MatchingEngine<Book, RecordingSink> e(b, s);
    e.process(mk(1, Side::Buy, 100, 10));
    e.process(mk(2, Side::Sell, 60, 10));
    e.process(mk(3, Side::Sell, 50, 10));
    e.process(mk(4, Side::Buy, 102, 10));
    CHECK(s.trades.size() == 2);
    CHECK(s.trades[0].price == 100 && s.trades[1].price == 50);
    CHECK(s.trades[1].buy_order_id == 4 && s.trades[1].sell_order_id == 3);
    CHECK(e.tradeCount() == 2);
  }
  {  // partial fill of the RESTING order, then completion
    Book b; CountingSink s; MatchingEngine<Book, CountingSink> e(b, s);
    e.process(mk(1, Side::Buy, 100, 10));
    e.process(mk(2, Side::Sell, 100, 4));
    CHECK(b.frontAtBestBid()->quantity == 6 && s.partial_fills == 1 && s.full_fills == 1);
    e.process(mk(3, Side::Sell, 100, 6));
    CHECK(b.orderCount() == 0 && s.trades == 2 && s.traded_quantity == 10);
  }
  {  // incoming sweeps levels, partially filled remainder rests
    Book b; RecordingSink s; MatchingEngine<Book, RecordingSink> e(b, s);
    e.process(mk(1, Side::Sell, 101, 5));
    e.process(mk(2, Side::Sell, 102, 5));
    e.process(mk(3, Side::Sell, 103, 5));
    e.process(mk(10, Side::Buy, 102, 12));
    CHECK(s.trades.size() == 2);
    CHECK(s.trades[0].price == 101 && s.trades[1].price == 102);
    CHECK(b.bestBid() == 102 && b.frontAtBestBid()->quantity == 2);
    CHECK(b.bestAsk() == 103);
    int partial_for_10 = 0;
    for (auto& ev : s.events)
      if (ev.type == EventType::OrderPartiallyFilled && ev.order_id == 10) ++partial_for_10;
    CHECK(partial_for_10 == 1);
  }
  {  // FIFO at the same price
    Book b; RecordingSink s; MatchingEngine<Book, RecordingSink> e(b, s);
    e.process(mk(1, Side::Sell, 100, 5));
    e.process(mk(2, Side::Sell, 100, 5));
    e.process(mk(3, Side::Buy, 100, 5));
    CHECK(s.trades.size() == 1 && s.trades[0].sell_order_id == 1);
    CHECK(b.frontAtBestAsk()->id == 2);
  }
  {  // rejections: zero qty, duplicate resting id
    Book b; CountingSink s; MatchingEngine<Book, CountingSink> e(b, s);
    e.process(mk(1, Side::Buy, 100, 0));
    CHECK(s.rejected == 1 && s.accepted == 0);
    e.process(mk(2, Side::Buy, 100, 5));
    e.process(mk(2, Side::Sell, 150, 5));  // duplicate of a resting id
    CHECK(s.rejected == 2 && s.accepted == 1 && b.orderCount() == 1);
  }
  std::printf("  scenarios OK (%s)\n", name);
}

static void limitsOnFastBook() {
  using B = OrderBookFast<2, 64>;
  B b(100);
  CountingSink s;
  MatchingEngine<B, CountingSink> e(b, s);
  e.process(mk(1, Side::Buy, 500, 5));   // out of range -> rejected, book untouched
  CHECK(s.rejected == 1 && b.orderCount() == 0);
  e.process(mk(2, Side::Buy, 110, 5));
  e.process(mk(3, Side::Buy, 111, 5));
  e.process(mk(4, Side::Buy, 112, 5));   // pool full -> remainder rejected
  CHECK(s.rejected == 2 && e.rejectedRemainders() == 1 && b.orderCount() == 2);
  std::puts("  fast-book limits OK (range + pool exhaustion)");
}

static void v1VsV3Determinism() {
  OrderBook v1;
  OrderBookFast<(1u << 18), 8192> v3(/*min_price=*/6000);
  CountingSink s1, s3;
  MatchingEngine<OrderBook, CountingSink> e1(v1, s1);
  MatchingEngine<OrderBookFast<(1u << 18), 8192>, CountingSink> e3(v3, s3);

  std::mt19937_64 rng(99);
  std::normal_distribution<double> off(0.0, 25.0);
  for (OrderId id = 1; id <= 300000; ++id) {
    Price p = 10000 + static_cast<Price>(off(rng));
    Order o = mk(id, (rng() & 1) ? Side::Buy : Side::Sell, p, 1 + rng() % 100);
    e1.process(o);
    e3.process(o);
  }
  CHECK(s1.trades == s3.trades && s1.traded_quantity == s3.traded_quantity);
  CHECK(s1.traded_notional == s3.traded_notional);
  CHECK(s1.accepted == s3.accepted && s1.rejected == s3.rejected);
  CHECK(s1.partial_fills == s3.partial_fills && s1.full_fills == s3.full_fills);
  CHECK(v1.orderCount() == v3.orderCount());
  std::printf("  V1 == V3 on 300k random orders (%llu trades)\n",
              static_cast<unsigned long long>(s1.trades));
}

int main() {
  std::puts("MatchingEngine:");
  scenarioTests<OrderBook>("V1");
  scenarioTests<OrderBookFast<1024, 1024>>("V3");
  limitsOnFastBook();
  v1VsV3Determinism();
  std::puts("All matching engine tests passed.");
  return 0;
}
