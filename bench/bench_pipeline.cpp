// End-to-end pipeline benchmark:
//
//   producer thread --Order--> [Queue] --Order--> engine thread (MatchingEngine + Book)
//
// Matrix:  {mutex RingBuffer, SPSC RingBuffer}  x  {OrderBook V1, OrderBookFast V3}
//
// Latency of one order = (time the engine finished processing it)
//                        - (order.submit_time, stamped by the producer right
//                           before push)       => includes queueing delay.
//
// Two runs per configuration:
//   throughput : producer unthrottled (queue stays full) -> max orders/sec
//   latency    : producer paced at --rate orders/sec     -> p50/p99/p99.9
//
// Build (one line):
//   g++ -std=c++17 -O2 -DNDEBUG -Iinclude src/order_book.cpp src/ring_buffer.cpp bench/bench_pipeline.cpp -o bench_pipeline -pthread
// Run:    ./bench_pipeline [orders=2000000] [rate=500000] [cpu_producer=-1] [cpu_engine=-1]
//   e.g.  ./bench_pipeline 2000000 500000 2 3        (pin threads, Linux)
//
// NOTE: needs >= 2 free CPU cores for meaningful numbers. On 1 core the
// two threads time-slice each other and latency is meaningless.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "matching_engine.hpp"
#include "order_book.hpp"
#include "order_book_fast.hpp"
#include "ring_buffer.hpp"
#include "spsc_ring_buffer.hpp"

using Clock = std::chrono::steady_clock;

static void pinThisThread(int cpu) {
#if defined(__linux__)
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)cpu;
#endif
}

static std::vector<Order> makeOrders(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> off(0.0, 20.0);
  std::vector<Order> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    Order& o = v[i];
    o.id = i + 1;
    o.side = (rng() & 1) ? Side::Buy : Side::Sell;
    o.price = 10000 + static_cast<Price>(off(rng));
    o.quantity = 1 + rng() % 100;
    o.submit_time = Timestamp{};
  }
  return v;
}

struct Result {
  double seconds = 0;
  double ops_per_sec = 0;
  std::vector<std::uint64_t> lat_ns;  // sorted
  CountingSink counts;
  std::size_t resting = 0;
};

template <class Queue, class Book>
static Result runOnce(const std::vector<Order>& orders, double rate,
                      int cpu_prod, int cpu_eng, bool record_latency) {
  const std::size_t n = orders.size();
  Result r;
  if (record_latency) r.lat_ns.resize(n);  // preallocated: no allocation in the loop

  auto queue = std::make_unique<Queue>();
  auto book = std::make_unique<Book>();
  CountingSink sink;
  MatchingEngine<Book, CountingSink> engine(*book, sink);

  std::atomic<bool> go{false};
  Clock::time_point t_start, t_end;

  std::thread producer([&] {
    pinThisThread(cpu_prod);
    while (!go.load(std::memory_order_acquire)) {}
    const double ns_per_order = rate > 0 ? 1e9 / rate : 0;
    const auto base = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      if (rate > 0) {  // open-loop pacing: order i is due at base + i * period
        const auto due = base + std::chrono::nanoseconds(
                                    static_cast<std::int64_t>(i * ns_per_order));
        while (Clock::now() < due) {}
      }
      Order o = orders[i];
      o.submit_time = Clock::now();  // "when the order entered the system"
      queue->push(o);
    }
  });

  std::thread consumer([&] {
    pinThisThread(cpu_eng);
    while (!go.load(std::memory_order_acquire)) {}
    t_start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      Order o = queue->pop();
      engine.process(o);
      if (record_latency) {
        r.lat_ns[i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - o.submit_time)
                .count());
      }
    }
    t_end = Clock::now();
  });

  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  r.seconds = std::chrono::duration<double>(t_end - t_start).count();
  r.ops_per_sec = static_cast<double>(n) / r.seconds;
  r.counts = sink;
  r.resting = book->orderCount();
  if (record_latency) std::sort(r.lat_ns.begin(), r.lat_ns.end());
  return r;
}

static double pct(const std::vector<std::uint64_t>& v, double p) {
  return v.empty() ? 0 : static_cast<double>(v[static_cast<std::size_t>(p / 100.0 * (v.size() - 1))]);
}

using Fast = OrderBookFast<(1u << 20), (1u << 16)>;

template <class Queue, class Book>
static CountingSink benchConfig(const char* qname, const char* bname,
                                const std::vector<Order>& orders, double rate,
                                int cp, int ce) {
  // warmup (small) so page faults / cold caches do not pollute the real run
  {
    std::vector<Order> small(orders.begin(), orders.begin() + std::min<std::size_t>(orders.size(), 50000));
    runOnce<Queue, Book>(small, 0, cp, ce, false);
  }
  Result tp = runOnce<Queue, Book>(orders, 0, cp, ce, false);
  Result lt = runOnce<Queue, Book>(orders, rate, cp, ce, true);
  std::printf("%-6s %-4s | %8.2f M/s | %8.0f %8.0f %8.0f %9.0f %11.0f\n", qname, bname,
              tp.ops_per_sec / 1e6, pct(lt.lat_ns, 50), pct(lt.lat_ns, 99),
              pct(lt.lat_ns, 99.9), pct(lt.lat_ns, 100), lt.ops_per_sec / 1e6 * 1e3);
  std::fflush(stdout);
  return tp.counts;
}

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
  const double rate = argc > 2 ? std::strtod(argv[2], nullptr) : 500000;
  const int cp = argc > 3 ? std::atoi(argv[3]) : -1;
  const int ce = argc > 4 ? std::atoi(argv[4]) : -1;

  std::printf("orders=%zu  paced rate=%.0f/s  cpus: producer=%d engine=%d  (hw threads: %u)\n",
              n, rate, cp, ce, std::thread::hardware_concurrency());
  const auto orders = makeOrders(n, 42);

  std::printf("\n%-6s %-4s | %10s | %8s %8s %8s %9s %11s\n", "queue", "book",
              "throughput", "p50 ns", "p99 ns", "p99.9 ns", "max ns", "achvd k/s");
  std::printf("-----------+------------+--------------------------------------------------\n");

  CountingSink a = benchConfig<RingBuffer, OrderBook>("mutex", "V1", orders, rate, cp, ce);
  CountingSink b = benchConfig<RingBuffer, Fast>("mutex", "V3", orders, rate, cp, ce);
  CountingSink c = benchConfig<SpscRingBuffer<1024>, OrderBook>("spsc", "V1", orders, rate, cp, ce);
  CountingSink d = benchConfig<SpscRingBuffer<1024>, Fast>("spsc", "V3", orders, rate, cp, ce);

  // Correctness gate: all four configurations must produce IDENTICAL results.
  auto same = [](const CountingSink& x, const CountingSink& y) {
    return x.trades == y.trades && x.traded_quantity == y.traded_quantity &&
           x.traded_notional == y.traded_notional && x.accepted == y.accepted &&
           x.rejected == y.rejected;
  };
  const bool ok = same(a, b) && same(a, c) && same(a, d);
  std::printf("\nresult check: trades=%llu traded_qty=%llu  all 4 configs identical: %s\n",
              static_cast<unsigned long long>(a.trades),
              static_cast<unsigned long long>(a.traded_quantity), ok ? "YES" : "NO  <-- BUG");
  return ok ? 0 : 1;
}
