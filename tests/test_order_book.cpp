// Plain-assert tests for OrderBook (no test framework needed).
// Build & run:  see CMake snippet, or
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/order_book.cpp tests/test_order_book.cpp -o test_order_book

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "order_book.hpp"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Test-only helpers. The REAL matching loop belongs to the Matching Engine.
// This tiny copy exists only so we can verify the book supports it correctly.
// ---------------------------------------------------------------------------
struct TestTrade {
    OrderId  buy_id;
    OrderId  sell_id;
    Price    price;
    Quantity quantity;
};

static Order makeOrder(OrderId id, Side side, Price price, Quantity quantity) {
    Order o{};
    o.id = id;
    o.side = side;
    o.price = price;
    o.quantity = quantity;
    o.submit_time = Timestamp{};
    return o;
}

static void submit(OrderBook& book, Order in, std::vector<TestTrade>& trades) {
    if (in.side == Side::Buy) {
        while (in.quantity > 0 && book.hasAsk() && book.bestAsk() <= in.price) {
            Order* resting = book.frontAtBestAsk();
            OrderId  rid = resting->id;
            Price    rprice = resting->price;      // trade at RESTING price
            Quantity q = std::min(in.quantity, resting->quantity);
            trades.push_back({in.id, rid, rprice, q});
            in.quantity -= q;
            book.reduce(rid, q);                   // 'resting' may be invalid now
        }
    } else {
        while (in.quantity > 0 && book.hasBid() && book.bestBid() >= in.price) {
            Order* resting = book.frontAtBestBid();
            OrderId  rid = resting->id;
            Price    rprice = resting->price;
            Quantity q = std::min(in.quantity, resting->quantity);
            trades.push_back({rid, in.id, rprice, q});
            in.quantity -= q;
            book.reduce(rid, q);
        }
    }
    if (in.quantity > 0) book.add(in);
}

// ---------------------------------------------------------------------------

static void test_empty_book() {
    OrderBook b;
    CHECK(!b.hasBid());
    CHECK(!b.hasAsk());
    CHECK(b.frontAtBestBid() == nullptr);
    CHECK(b.frontAtBestAsk() == nullptr);
    CHECK(b.orderCount() == 0);
}

static void test_best_prices() {
    OrderBook b;
    b.add(makeOrder(1, Side::Buy, 98, 5));
    b.add(makeOrder(2, Side::Buy, 100, 5));
    b.add(makeOrder(3, Side::Buy, 99, 5));
    b.add(makeOrder(4, Side::Sell, 105, 5));
    b.add(makeOrder(5, Side::Sell, 103, 5));
    b.add(makeOrder(6, Side::Sell, 104, 5));

    CHECK(b.bestBid() == 100);  // highest buy
    CHECK(b.bestAsk() == 103);  // lowest sell
    CHECK(b.bidLevelCount() == 3);
    CHECK(b.askLevelCount() == 3);
}

static void test_time_priority_same_price() {
    OrderBook b;
    b.add(makeOrder(1, Side::Sell, 100, 5));
    b.add(makeOrder(2, Side::Sell, 100, 5));
    b.add(makeOrder(3, Side::Sell, 100, 5));
    CHECK(b.frontAtBestAsk()->id == 1);  // oldest first

    CHECK(b.reduce(1, 5));               // fully filled -> removed
    CHECK(b.frontAtBestAsk()->id == 2);
}

static void test_partial_reduce_keeps_position() {
    OrderBook b;
    b.add(makeOrder(1, Side::Buy, 100, 10));
    b.add(makeOrder(2, Side::Buy, 100, 10));

    CHECK(b.reduce(1, 4));
    CHECK(b.frontAtBestBid()->id == 1);   // still first in queue
    CHECK(b.frontAtBestBid()->quantity == 6);
    CHECK(b.orderCount() == 2);
}

static void test_reduce_invalid() {
    OrderBook b;
    b.add(makeOrder(1, Side::Buy, 100, 10));
    CHECK(!b.reduce(99, 1));   // unknown id
    CHECK(!b.reduce(1, 0));    // zero
    CHECK(!b.reduce(1, 11));   // more than resting
    CHECK(b.frontAtBestBid()->quantity == 10);
}

static void test_level_removed_and_best_updates() {
    OrderBook b;
    b.add(makeOrder(1, Side::Buy, 100, 5));
    b.add(makeOrder(2, Side::Buy, 99, 5));
    CHECK(b.bestBid() == 100);
    CHECK(b.reduce(1, 5));
    CHECK(b.bidLevelCount() == 1);
    CHECK(b.bestBid() == 99);
    CHECK(b.cancel(2));
    CHECK(!b.hasBid());
}

static void test_cancel_first_middle_last() {
    OrderBook b;
    for (OrderId i = 1; i <= 5; ++i) b.add(makeOrder(i, Side::Sell, 100, 1));

    CHECK(b.cancel(3));                       // middle
    CHECK(b.cancel(1));                       // first
    CHECK(b.cancel(5));                       // last
    CHECK(b.orderCount() == 2);
    CHECK(b.frontAtBestAsk()->id == 2);       // order preserved: 2 then 4
    CHECK(b.reduce(2, 1));
    CHECK(b.frontAtBestAsk()->id == 4);

    CHECK(!b.cancel(3));                      // already gone
    CHECK(!b.cancel(12345));                  // never existed
}

static void test_rejects() {
    OrderBook b;
    CHECK(b.add(makeOrder(1, Side::Buy, 100, 5)));
    CHECK(!b.add(makeOrder(1, Side::Sell, 200, 5)));  // duplicate id
    CHECK(!b.add(makeOrder(2, Side::Buy, 100, 0)));   // zero quantity
    CHECK(b.orderCount() == 1);
    CHECK(!b.hasAsk());
}

// Your example, arrival order B1, S1, S2, B2
static void test_example_case1() {
    OrderBook b;
    std::vector<TestTrade> t;
    submit(b, makeOrder(1, Side::Buy, 100, 10), t);   // B1 rests
    submit(b, makeOrder(2, Side::Sell, 60, 10), t);   // S1 hits B1 @100
    submit(b, makeOrder(3, Side::Sell, 50, 10), t);   // S2 rests
    submit(b, makeOrder(4, Side::Buy, 102, 10), t);   // B2 hits S2 @50

    CHECK(t.size() == 2);
    CHECK(t[0].buy_id == 1 && t[0].sell_id == 2 && t[0].price == 100 && t[0].quantity == 10);
    CHECK(t[1].buy_id == 4 && t[1].sell_id == 3 && t[1].price == 50 && t[1].quantity == 10);
    CHECK(b.orderCount() == 0);
}

// Your example, arrival order S1, S2, B1, B2
static void test_example_case2() {
    OrderBook b;
    std::vector<TestTrade> t;
    submit(b, makeOrder(2, Side::Sell, 60, 10), t);   // S1 rests
    submit(b, makeOrder(3, Side::Sell, 50, 10), t);   // S2 rests
    submit(b, makeOrder(1, Side::Buy, 100, 10), t);   // B1 takes best ask = S2 @50
    submit(b, makeOrder(4, Side::Buy, 102, 10), t);   // B2 takes S1 @60

    CHECK(t.size() == 2);
    CHECK(t[0].buy_id == 1 && t[0].sell_id == 3 && t[0].price == 50);
    CHECK(t[1].buy_id == 4 && t[1].sell_id == 2 && t[1].price == 60);
    CHECK(b.orderCount() == 0);
}

static void test_sweep_multiple_levels_and_rest_remainder() {
    OrderBook b;
    std::vector<TestTrade> t;
    submit(b, makeOrder(1, Side::Sell, 101, 5), t);
    submit(b, makeOrder(2, Side::Sell, 102, 5), t);
    submit(b, makeOrder(3, Side::Sell, 103, 5), t);

    // Buy 12 @102: takes 5@101, 5@102, can't take 103 -> 2 left rests as a bid.
    submit(b, makeOrder(10, Side::Buy, 102, 12), t);

    CHECK(t.size() == 2);
    CHECK(t[0].price == 101 && t[0].quantity == 5);
    CHECK(t[1].price == 102 && t[1].quantity == 5);
    CHECK(b.bestBid() == 102);
    CHECK(b.frontAtBestBid()->quantity == 2);
    CHECK(b.bestAsk() == 103);
}

static void test_partial_fill_of_resting_then_next_incoming() {
    OrderBook b;
    std::vector<TestTrade> t;
    submit(b, makeOrder(1, Side::Buy, 100, 10), t);
    submit(b, makeOrder(2, Side::Sell, 100, 4), t);   // B1 left with 6
    submit(b, makeOrder(3, Side::Sell, 100, 6), t);   // B1 fully filled

    CHECK(t.size() == 2);
    CHECK(t[0].quantity == 4 && t[1].quantity == 6);
    CHECK(b.orderCount() == 0);
}

int main() {
    test_empty_book();
    test_best_prices();
    test_time_priority_same_price();
    test_partial_reduce_keeps_position();
    test_reduce_invalid();
    test_level_removed_and_best_updates();
    test_cancel_first_middle_last();
    test_rejects();
    test_example_case1();
    test_example_case2();
    test_sweep_multiple_levels_and_rest_remainder();
    test_partial_fill_of_resting_then_next_incoming();
    std::puts("All order book tests passed.");
    return 0;
}