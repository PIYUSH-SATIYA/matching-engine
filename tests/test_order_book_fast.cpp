// Tests for OrderBookFast (V3).
//   1. The SAME unit tests are run on V1 and on V3 (same interface).
//   2. Differential test: random adds / cancels, V3 must behave exactly like V1.
//   3. Hash-table stress (collisions + backward-shift deletion).
//   4. Limits: out-of-range price, pool exhaustion.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>

#include "order_book.hpp"
#include "order_book_fast.hpp"

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

static Order makeOrder(OrderId id, Side side, Price price, Quantity qty) {
  Order o{};
  o.id = id;
  o.side = side;
  o.price = price;
  o.quantity = qty;
  o.submit_time = Timestamp{};
  return o;
}

struct TestTrade {
  OrderId buy_id, sell_id;
  Price price;
  Quantity quantity;
  bool operator==(const TestTrade& o) const {
    return buy_id == o.buy_id && sell_id == o.sell_id && price == o.price &&
           quantity == o.quantity;
  }
};

template <class Book>
static void submit(Book& b, Order in, std::vector<TestTrade>& trades) {
  if (!b.validPrice(in.price)) return;
  if (in.side == Side::Buy) {
    while (in.quantity > 0 && b.hasAsk() && b.bestAsk() <= in.price) {
      Order* r = b.frontAtBestAsk();
      OrderId rid = r->id;
      Price rp = r->price;
      Quantity q = std::min(in.quantity, r->quantity);
      trades.push_back({in.id, rid, rp, q});
      in.quantity -= q;
      b.reduce(rid, q);
    }
  } else {
    while (in.quantity > 0 && b.hasBid() && b.bestBid() >= in.price) {
      Order* r = b.frontAtBestBid();
      OrderId rid = r->id;
      Price rp = r->price;
      Quantity q = std::min(in.quantity, r->quantity);
      trades.push_back({rid, in.id, rp, q});
      in.quantity -= q;
      b.reduce(rid, q);
    }
  }
  if (in.quantity > 0) b.add(in);
}

// ------------------------------------------------- shared unit tests (V1 + V3)
template <class Book>
static void unitTests(const char* name) {
  {  // empty
    Book b;
    CHECK(!b.hasBid() && !b.hasAsk());
    CHECK(b.frontAtBestBid() == nullptr && b.frontAtBestAsk() == nullptr);
    CHECK(b.orderCount() == 0);
  }
  {  // best prices + level counts
    Book b;
    b.add(makeOrder(1, Side::Buy, 98, 5));
    b.add(makeOrder(2, Side::Buy, 100, 5));
    b.add(makeOrder(3, Side::Buy, 99, 5));
    b.add(makeOrder(4, Side::Sell, 105, 5));
    b.add(makeOrder(5, Side::Sell, 103, 5));
    b.add(makeOrder(6, Side::Sell, 104, 5));
    CHECK(b.bestBid() == 100 && b.bestAsk() == 103);
    CHECK(b.bidLevelCount() == 3 && b.askLevelCount() == 3);
  }
  {  // time priority + partial reduce keeps place
    Book b;
    b.add(makeOrder(1, Side::Sell, 100, 10));
    b.add(makeOrder(2, Side::Sell, 100, 10));
    b.add(makeOrder(3, Side::Sell, 100, 10));
    CHECK(b.frontAtBestAsk()->id == 1);
    CHECK(b.reduce(1, 4));
    CHECK(b.frontAtBestAsk()->id == 1 && b.frontAtBestAsk()->quantity == 6);
    CHECK(b.reduce(1, 6));
    CHECK(b.frontAtBestAsk()->id == 2);
  }
  {  // invalid reduce
    Book b;
    b.add(makeOrder(1, Side::Buy, 100, 10));
    CHECK(!b.reduce(99, 1) && !b.reduce(1, 0) && !b.reduce(1, 11));
    CHECK(b.frontAtBestBid()->quantity == 10);
  }
  {  // level removal updates best (including gaps between levels)
    Book b;
    b.add(makeOrder(1, Side::Buy, 100, 5));
    b.add(makeOrder(2, Side::Buy, 90, 5));
    b.add(makeOrder(3, Side::Sell, 120, 5));
    b.add(makeOrder(4, Side::Sell, 130, 5));
    CHECK(b.reduce(1, 5));
    CHECK(b.bestBid() == 90);
    CHECK(b.cancel(3));
    CHECK(b.bestAsk() == 130);
    CHECK(b.cancel(2) && !b.hasBid());
    CHECK(b.cancel(4) && !b.hasAsk());
    CHECK(b.orderCount() == 0);
  }
  {  // cancel first / middle / last
    Book b;
    for (OrderId i = 1; i <= 5; ++i) b.add(makeOrder(i, Side::Sell, 100, 1));
    CHECK(b.cancel(3) && b.cancel(1) && b.cancel(5));
    CHECK(b.orderCount() == 2 && b.frontAtBestAsk()->id == 2);
    CHECK(b.reduce(2, 1) && b.frontAtBestAsk()->id == 4);
    CHECK(!b.cancel(3) && !b.cancel(12345));
  }
  {  // rejects
    Book b;
    CHECK(b.add(makeOrder(1, Side::Buy, 100, 5)));
    CHECK(!b.add(makeOrder(1, Side::Sell, 200, 5)));  // duplicate id
    CHECK(!b.add(makeOrder(2, Side::Buy, 100, 0)));   // zero qty
    CHECK(b.orderCount() == 1 && !b.hasAsk());
  }
  {  // user's example, arrival B1 S1 S2 B2
    Book b;
    std::vector<TestTrade> t;
    submit(b, makeOrder(1, Side::Buy, 100, 10), t);
    submit(b, makeOrder(2, Side::Sell, 60, 10), t);
    submit(b, makeOrder(3, Side::Sell, 50, 10), t);
    submit(b, makeOrder(4, Side::Buy, 102, 10), t);
    CHECK(t.size() == 2);
    CHECK(t[0] == (TestTrade{1, 2, 100, 10}));
    CHECK(t[1] == (TestTrade{4, 3, 50, 10}));
    CHECK(b.orderCount() == 0);
  }
  {  // sweep several levels, remainder rests
    Book b;
    std::vector<TestTrade> t;
    submit(b, makeOrder(1, Side::Sell, 101, 5), t);
    submit(b, makeOrder(2, Side::Sell, 102, 5), t);
    submit(b, makeOrder(3, Side::Sell, 103, 5), t);
    submit(b, makeOrder(10, Side::Buy, 102, 12), t);
    CHECK(t.size() == 2 && t[0].price == 101 && t[1].price == 102);
    CHECK(b.bestBid() == 102 && b.frontAtBestBid()->quantity == 2);
    CHECK(b.bestAsk() == 103);
  }
  std::printf("  unit tests OK  (%s)\n", name);
}

// ------------------------------------------------------------ differential
using Fast = OrderBookFast<(1u << 18), 4096>;

template <class A, class B>
static void sameState(A& a, B& b) {
  CHECK(a.hasBid() == b.hasBid());
  CHECK(a.hasAsk() == b.hasAsk());
  CHECK(a.orderCount() == b.orderCount());
  CHECK(a.bidLevelCount() == b.bidLevelCount());
  CHECK(a.askLevelCount() == b.askLevelCount());
  if (a.hasBid()) {
    CHECK(a.bestBid() == b.bestBid());
    CHECK(a.frontAtBestBid()->id == b.frontAtBestBid()->id);
    CHECK(a.frontAtBestBid()->quantity == b.frontAtBestBid()->quantity);
  }
  if (a.hasAsk()) {
    CHECK(a.bestAsk() == b.bestAsk());
    CHECK(a.frontAtBestAsk()->id == b.frontAtBestAsk()->id);
    CHECK(a.frontAtBestAsk()->quantity == b.frontAtBestAsk()->quantity);
  }
}

static void differentialTest(std::uint64_t seed, std::size_t ops, double sd) {
  OrderBook ref;
  Fast fast(/*min_price=*/4000);  // range [4000, 8096): also tests the offset
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> off(0.0, sd);
  OrderId next_id = 1;
  std::vector<TestTrade> ta, tb;

  for (std::size_t i = 0; i < ops; ++i) {
    const unsigned r = static_cast<unsigned>(rng() % 100);
    if (r < 65 || next_id == 1) {
      Price p = 5000 + static_cast<Price>(off(rng));
      p = std::max<Price>(4100, std::min<Price>(7900, p));  // stay inside fast range
      Order o = makeOrder(next_id++, (rng() & 1) ? Side::Buy : Side::Sell, p,
                          1 + static_cast<Quantity>(rng() % 100));
      ta.clear();
      tb.clear();
      submit(ref, o, ta);
      submit(fast, o, tb);
      CHECK(ta == tb);
    } else if (r < 90) {
      OrderId id = 1 + rng() % (next_id - 1);
      CHECK(ref.cancel(id) == fast.cancel(id));
    } else {
      OrderId id = 1 + rng() % (next_id - 1);
      Quantity q = 1 + static_cast<Quantity>(rng() % 50);
      CHECK(ref.reduce(id, q) == fast.reduce(id, q));
    }
    sameState(ref, fast);
  }
  std::printf("  differential OK (seed %llu, %zu ops, sd %.0f, resting at end %zu)\n",
              static_cast<unsigned long long>(seed), ops, sd, ref.orderCount());
}

// ------------------------------------------------- hash collision / deletion
static void hashStress() {
  OrderBookFast<64, 128> b;  // tiny hash table (128 slots) => many collisions
  std::set<OrderId> live;
  std::mt19937_64 rng(7);
  for (int round = 0; round < 20000; ++round) {
    if (live.size() < 64 && (rng() % 100 < 55 || live.empty())) {
      OrderId id = 1 + rng() % 100000;
      if (b.add(makeOrder(id, (id & 1) ? Side::Buy : Side::Sell,
                          (id & 1) ? 10 + id % 20 : 40 + id % 20, 1 + id % 9))) {
        CHECK(live.insert(id).second);
      } else {
        CHECK(live.count(id) == 1);  // only failure reason here: duplicate
      }
    } else {
      auto it = live.begin();
      std::advance(it, static_cast<long>(rng() % live.size()));
      CHECK(b.cancel(*it));
      live.erase(it);
    }
    CHECK(b.orderCount() == live.size());
  }
  for (OrderId id = 1; id <= 100000; id += 97) CHECK(b.contains(id) == (live.count(id) == 1));
  std::printf("  hash stress OK\n");
}

static void limitsTest() {
  OrderBookFast<4, 64> b(100);  // prices [100, 164), 4 orders max
  CHECK(!b.validPrice(99) && b.validPrice(100) && b.validPrice(163) && !b.validPrice(164));
  CHECK(!b.add(makeOrder(1, Side::Buy, 99, 1)));
  CHECK(!b.add(makeOrder(1, Side::Buy, 164, 1)));
  CHECK(b.add(makeOrder(1, Side::Buy, 100, 1)));
  CHECK(b.add(makeOrder(2, Side::Buy, 163, 1)));
  CHECK(b.bestBid() == 163);
  CHECK(b.add(makeOrder(3, Side::Sell, 120, 1)));
  CHECK(b.add(makeOrder(4, Side::Sell, 121, 1)));
  CHECK(!b.add(makeOrder(5, Side::Sell, 130, 1)));  // pool exhausted
  CHECK(b.cancel(4));
  CHECK(b.add(makeOrder(5, Side::Sell, 130, 1)));   // space is reusable
  CHECK(b.bestAsk() == 120);
  std::printf("  limits OK (range check, pool exhaustion, reuse)\n");
}

int main() {
  std::puts("V1 OrderBook:");
  unitTests<OrderBook>("V1");
  std::puts("V3 OrderBookFast:");
  unitTests<OrderBookFast<1024, 1024>>("V3");
  differentialTest(1, 300000, 15.0);
  differentialTest(2, 300000, 400.0);  // wide spread -> many levels, level gaps
  hashStress();
  limitsTest();
  std::puts("All OrderBookFast tests passed.");
  return 0;
}
