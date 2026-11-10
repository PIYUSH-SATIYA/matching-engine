// Order book micro-benchmark: V1 (map+list+unordered_map) vs V3 (OrderBookFast).
// Single thread, no queue, no network: measures ONLY the book + matching loop.
//
// Build (one line):
//   g++ -std=c++17 -O2 -DNDEBUG -Iinclude src/order_book.cpp bench/bench_order_book.cpp -o bench_order_book
// Run:
//   taskset -c 2 ./bench_order_book [num_ops=2000000] [seed=42]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#include "order_book.hpp"
#include "order_book_fast.hpp"

using Clock = std::chrono::steady_clock;

enum class OpKind : std::uint8_t { NewOrder, Cancel };
struct Op {
  OpKind kind;
  Order order;
  OrderId cancel_id;
};

static std::vector<Op> generateWorkload(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> offset(0.0, 20.0);
  std::uniform_int_distribution<int> side_dist(0, 1), qty_dist(1, 100), kind_dist(0, 99);
  std::vector<Op> ops;
  ops.reserve(n);
  OrderId next_id = 1;
  for (std::size_t i = 0; i < n; ++i) {
    Op op{};
    if (kind_dist(rng) < 70 || next_id == 1) {
      op.kind = OpKind::NewOrder;
      op.order.id = next_id++;
      op.order.side = side_dist(rng) ? Side::Buy : Side::Sell;
      op.order.price = 10000 + static_cast<Price>(offset(rng));
      op.order.quantity = static_cast<Quantity>(qty_dist(rng));
      op.order.submit_time = Timestamp{};
    } else {
      op.kind = OpKind::Cancel;  // may target an already-filled order -> miss
      op.cancel_id = std::uniform_int_distribution<OrderId>(1, next_id - 1)(rng);
    }
    ops.push_back(op);
  }
  return ops;
}

struct Counters {
  std::uint64_t trades = 0, traded_qty = 0, cancels_ok = 0, cancels_miss = 0;
};

template <class Book>
static inline void apply(Book& book, const Op& op, Counters& c) {
  if (op.kind == OpKind::Cancel) {
    if (book.cancel(op.cancel_id)) ++c.cancels_ok; else ++c.cancels_miss;
    return;
  }
  Order in = op.order;
  if (in.side == Side::Buy) {
    while (in.quantity > 0 && book.hasAsk() && book.bestAsk() <= in.price) {
      Order* r = book.frontAtBestAsk();
      OrderId rid = r->id;
      Quantity q = std::min(in.quantity, r->quantity);
      in.quantity -= q; ++c.trades; c.traded_qty += q;
      book.reduce(rid, q);
    }
  } else {
    while (in.quantity > 0 && book.hasBid() && book.bestBid() >= in.price) {
      Order* r = book.frontAtBestBid();
      OrderId rid = r->id;
      Quantity q = std::min(in.quantity, r->quantity);
      in.quantity -= q; ++c.trades; c.traded_qty += q;
      book.reduce(rid, q);
    }
  }
  if (in.quantity > 0) book.add(in);
}

static double pct(const std::vector<std::uint64_t>& v, double p) {
  return static_cast<double>(v[static_cast<std::size_t>(p / 100.0 * (v.size() - 1))]);
}

template <class Book>
static Counters bench(const char* name, const std::vector<Op>& ops) {
  const std::size_t n = ops.size();
  { auto b = std::make_unique<Book>(); Counters c; for (auto& op : ops) apply(*b, op, c); }  // warmup

  Counters tp;
  double seconds;
  {  // pass 1: throughput, no per-op clock reads
    auto b = std::make_unique<Book>();
    auto t0 = Clock::now();
    for (auto& op : ops) apply(*b, op, tp);
    seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  }
  std::vector<std::uint64_t> lat;  // pass 2: per-op latency, sorted AFTER the run
  lat.reserve(n);
  {
    auto b = std::make_unique<Book>();
    Counters c;
    for (auto& op : ops) {
      auto a = Clock::now();
      apply(*b, op, c);
      auto z = Clock::now();
      lat.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(z - a).count()));
    }
  }
  std::sort(lat.begin(), lat.end());
  std::printf("%-26s %7.2f M/s | %6.0f %6.0f %7.0f %8.0f %10llu\n", name,
              static_cast<double>(n) / seconds / 1e6, pct(lat, 50), pct(lat, 90),
              pct(lat, 99), pct(lat, 99.9), static_cast<unsigned long long>(lat.back()));
  return tp;
}

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 42;
  std::printf("ops=%zu seed=%llu (70%% new orders / 30%% cancels)\n", n,
              static_cast<unsigned long long>(seed));
  const auto ops = generateWorkload(n, seed);

  std::printf("\n%-26s %13s | %6s %6s %7s %8s %10s   (latency in ns, incl. ~25 ns clock cost)\n",
              "book", "throughput", "p50", "p90", "p99", "p99.9", "max");
  Counters a = bench<OrderBook>("V1 map+list+unordered_map", ops);
  Counters b = bench<OrderBookFast<(1u << 20), (1u << 16)>>("V3 array+slab+bitmap", ops);

  const bool same = a.trades == b.trades && a.traded_qty == b.traded_qty &&
                    a.cancels_ok == b.cancels_ok;
  std::printf("\ntrades=%llu cancels ok/miss=%llu/%llu | V1 == V3 results: %s\n",
              static_cast<unsigned long long>(a.trades),
              static_cast<unsigned long long>(a.cancels_ok),
              static_cast<unsigned long long>(a.cancels_miss), same ? "YES" : "NO <-- BUG");
  return same ? 0 : 1;
}
