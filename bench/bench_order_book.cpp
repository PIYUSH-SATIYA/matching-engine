// Order book micro-benchmark (baseline for V1).
//
// Build (ALWAYS optimized, never Debug):
//   g++ -std=c++17 -O2 -DNDEBUG -Iinclude src/order_book.cpp bench/bench_order_book.cpp -o bench_order_book
// Run:
//   ./bench_order_book [num_ops] [seed]
//   taskset -c 2 ./bench_order_book 2000000      # pin to one core (Linux)
//
// What it measures: ONLY the order book + a tiny matching loop. No network,
// no ring buffer, no logging. The workload is generated BEFORE timing starts,
// so random-number generation is not counted.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "order_book.hpp"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- workload
enum class OpKind : std::uint8_t { NewOrder, Cancel };

struct Op {
    OpKind  kind;
    Order   order;      // used for NewOrder
    OrderId cancel_id;  // used for Cancel
};

static std::vector<Op> generateWorkload(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double>        offset(0.0, 20.0);   // ticks from mid
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      qty_dist(1, 100);
    std::uniform_int_distribution<int>      kind_dist(0, 99);

    constexpr Price kMid = 10000;
    std::vector<Op> ops;
    ops.reserve(n);

    OrderId next_id = 1;
    for (std::size_t i = 0; i < n; ++i) {
        Op op{};
        // ~70% new orders, ~30% cancels (cancel of an earlier id; it may
        // already be filled, in which case cancel() simply returns false).
        if (kind_dist(rng) < 70 || next_id == 1) {
            op.kind = OpKind::NewOrder;
            op.order.id = next_id++;
            op.order.side = side_dist(rng) ? Side::Buy : Side::Sell;
            op.order.price = kMid + static_cast<Price>(offset(rng));
            op.order.quantity = static_cast<Quantity>(qty_dist(rng));
            op.order.submit_time = Timestamp{};
        } else {
            op.kind = OpKind::Cancel;
            std::uniform_int_distribution<OrderId> pick(1, next_id - 1);
            op.cancel_id = pick(rng);
        }
        ops.push_back(op);
    }
    return ops;
}

// ------------------------------------------------- minimal matching loop
// (The real one belongs to the Matching Engine. This copy exists so the book
// is exercised the same way the engine will use it.)
struct Counters {
    std::uint64_t trades = 0;
    std::uint64_t traded_qty = 0;
    std::uint64_t cancels_ok = 0;
    std::uint64_t cancels_miss = 0;
};

static inline void apply(OrderBook& book, const Op& op, Counters& c) {
    if (op.kind == OpKind::Cancel) {
        if (book.cancel(op.cancel_id)) ++c.cancels_ok; else ++c.cancels_miss;
        return;
    }

    Order in = op.order;
    if (in.side == Side::Buy) {
        while (in.quantity > 0 && book.hasAsk() && book.bestAsk() <= in.price) {
            Order* r = book.frontAtBestAsk();
            OrderId  rid = r->id;
            Quantity q = std::min(in.quantity, r->quantity);
            in.quantity -= q;
            ++c.trades;
            c.traded_qty += q;
            book.reduce(rid, q);
        }
    } else {
        while (in.quantity > 0 && book.hasBid() && book.bestBid() >= in.price) {
            Order* r = book.frontAtBestBid();
            OrderId  rid = r->id;
            Quantity q = std::min(in.quantity, r->quantity);
            in.quantity -= q;
            ++c.trades;
            c.traded_qty += q;
            book.reduce(rid, q);
        }
    }
    if (in.quantity > 0) book.add(in);
}

// ------------------------------------------------------------------ stats
static double percentile(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * (sorted.size() - 1));
    return static_cast<double>(sorted[idx]);
}

static double clockOverheadNs() {
    constexpr int N = 100000;
    std::vector<std::uint64_t> d;
    d.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto a = Clock::now();
        auto b = Clock::now();
        d.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[d.size() / 2]);
}

int main(int argc, char** argv) {
    std::size_t   n    = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 42;

    std::printf("Generating workload: %zu ops, seed %llu ...\n", n,
                static_cast<unsigned long long>(seed));
    const std::vector<Op> ops = generateWorkload(n, seed);

    // ---- Warmup (fills caches, faults in memory, trains branch predictor) ----
    {
        OrderBook warm;
        Counters c;
        for (const Op& op : ops) apply(warm, op, c);
    }

    // ---- Pass 1: throughput (no per-op timing, so no clock overhead) ----
    Counters tp;
    double seconds;
    std::size_t final_orders;
    {
        OrderBook book;
        auto t0 = Clock::now();
        for (const Op& op : ops) apply(book, op, tp);
        auto t1 = Clock::now();
        seconds = std::chrono::duration<double>(t1 - t0).count();
        final_orders = book.orderCount();
    }

    // ---- Pass 2: per-operation latency (preallocated array, sorted AFTER) ----
    std::vector<std::uint64_t> lat;
    lat.reserve(n);
    {
        OrderBook book;
        Counters c;
        for (const Op& op : ops) {
            auto a = Clock::now();
            apply(book, op, c);
            auto b = Clock::now();
            lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
    }
    std::sort(lat.begin(), lat.end());

    const double overhead = clockOverheadNs();

    std::printf("\n=== Order Book V1 (std::map + std::list + unordered_map) ===\n");
    std::printf("ops                : %zu\n", n);
    std::printf("trades             : %llu\n", static_cast<unsigned long long>(tp.trades));
    std::printf("cancels ok / miss  : %llu / %llu\n",
                static_cast<unsigned long long>(tp.cancels_ok),
                static_cast<unsigned long long>(tp.cancels_miss));
    std::printf("resting at end     : %zu\n", final_orders);
    std::printf("throughput         : %.2f M ops/sec  (%.3f s total)\n",
                static_cast<double>(n) / seconds / 1e6, seconds);
    std::printf("\nper-op latency (ns), includes ~%.0f ns clock overhead:\n", overhead);
    std::printf("  p50    : %.0f\n", percentile(lat, 50));
    std::printf("  p90    : %.0f\n", percentile(lat, 90));
    std::printf("  p99    : %.0f\n", percentile(lat, 99));
    std::printf("  p99.9  : %.0f\n", percentile(lat, 99.9));
    std::printf("  max    : %llu\n", static_cast<unsigned long long>(lat.back()));
    return 0;
}