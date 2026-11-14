// bench_false_sharing.cpp — measure false sharing on the SPSC handoff boundary.
//
// Two lock-free SPSC ring buffers with byte-identical push/pop logic differ
// only in memory layout:
//
//   padded : head_ and cached_tail_ share one cache line, tail_ and
//            cached_head_ share another, and the data buffer starts on its own
//            line.  This is the layout used by include/spsc_ring_buffer.hpp.
//   packed : head_, cached_tail_, tail_ and cached_head_ are adjacent with no
//            cache-line padding (natural size_t alignment), and the data buffer
//            follows immediately.  Producer and consumer therefore write to the
//            same lines even though they own different variables.
//
// Both buffers use monotonic counters (never wrap), release/acquire ordering,
// and the same spin-with-pause / periodic-yield wait policy as
// include/spsc_ring_buffer.hpp.  The header itself is not modified.
//
// Producer pushes Orders as fast as possible; consumer pops them.  The producer
// stamps Order::submit_time immediately before push; the consumer reports
// nowNs() - submit_time, i.e. queueing + handoff latency (clock cost included,
// roughly 20-25 ns per measurement, the same for both layouts).
//
// Build:
//   g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wformat=2 -Iinclude -Isrc
//       bench/bench_false_sharing.cpp -o bench_false_sharing -pthread
// Run:
//   ./bench_false_sharing [orders=2000000] [producer_cpu=6] [consumer_cpu=2]
//   CPU < 0 disables pinning for that side.
//
// Each layout runs once per process.  Exit status is 1 if either layout
// processed fewer than the requested number of orders.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "affinity.hpp"
#include "order.hpp"

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Clock helpers
// ---------------------------------------------------------------------------

static std::uint64_t nsSinceEpoch(Timestamp t) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch())
          .count());
}

static std::uint64_t nowNs() noexcept {
  return nsSinceEpoch(Clock::now());
}

static int currentCpu() noexcept {
#if defined(__linux__)
  const int cpu = sched_getcpu();
  return cpu >= 0 ? cpu : -1;
#else
  return -1;
#endif
}

// ---------------------------------------------------------------------------
// Spin-wait policy — same shape as include/spsc_ring_buffer.hpp
// ---------------------------------------------------------------------------

static inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

static void waitHint(unsigned spins) noexcept {
  if (spins < 4096) {
    cpuRelax();
    return;
  }
  // Yield once per 4096-spin block rather than on every iteration, which would
  // turn into a sched_yield storm when the peer shares the core.
  if ((spins & 0xFFFu) == 0) {
    std::this_thread::yield();
  } else {
    cpuRelax();
  }
}

// ---------------------------------------------------------------------------
// Padded layout: one cache line per ownership domain
// ---------------------------------------------------------------------------

template <std::size_t Capacity>
class SpscPadded {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "SpscPadded: Capacity must be a power of two");
  static constexpr std::size_t kMask = Capacity - 1;

 public:
  SpscPadded() = default;
  SpscPadded(const SpscPadded&) = delete;
  SpscPadded& operator=(const SpscPadded&) = delete;

  // Producer thread only.
  bool try_push(const Order& o) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ >= Capacity) {                      // looks full
      cached_head_ = head_.load(std::memory_order_acquire);  // refresh
      if (t - cached_head_ >= Capacity) return false;        // really full
    }
    buf_[t & kMask] = o;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  void push(const Order& o) noexcept {
    for (unsigned spins = 0; !try_push(o); ++spins) waitHint(spins);
  }

  // Consumer thread only.
  bool try_pop(Order& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {                                 // looks empty
      cached_tail_ = tail_.load(std::memory_order_acquire);  // refresh
      if (h == cached_tail_) return false;                   // really empty
    }
    out = buf_[h & kMask];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  Order pop() noexcept {
    Order o;
    for (unsigned spins = 0; !try_pop(o); ++spins) waitHint(spins);
    return o;
  }

 private:
  // Consumer-owned line.
  alignas(64) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};
  // Producer-owned line.
  alignas(64) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};
  // Data.
  alignas(64) Order buf_[Capacity]{};
};

// ---------------------------------------------------------------------------
// Packed layout: no padding, indices and data share lines
// ---------------------------------------------------------------------------

template <std::size_t Capacity>
class alignas(alignof(std::size_t)) SpscPacked {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "SpscPacked: Capacity must be a power of two");
  static constexpr std::size_t kMask = Capacity - 1;

 public:
  SpscPacked() = default;
  SpscPacked(const SpscPacked&) = delete;
  SpscPacked& operator=(const SpscPacked&) = delete;

  // Producer thread only.
  bool try_push(const Order& o) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ >= Capacity) {                      // looks full
      cached_head_ = head_.load(std::memory_order_acquire);  // refresh
      if (t - cached_head_ >= Capacity) return false;        // really full
    }
    buf_[t & kMask] = o;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  void push(const Order& o) noexcept {
    for (unsigned spins = 0; !try_push(o); ++spins) waitHint(spins);
  }

  // Consumer thread only.
  bool try_pop(Order& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {                                 // looks empty
      cached_tail_ = tail_.load(std::memory_order_acquire);  // refresh
      if (h == cached_tail_) return false;                   // really empty
    }
    out = buf_[h & kMask];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  Order pop() noexcept {
    Order o;
    for (unsigned spins = 0; !try_pop(o); ++spins) waitHint(spins);
    return o;
  }

 private:
  // All four counters are adjacent.  alignas(1) is not legal for an 8-byte
  // atomic (it cannot under-align the natural size_t alignment), so the class
  // uses natural alignment and relies on the absence of 64-byte padding.
  std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};
  std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};
  Order buf_[Capacity]{};
};

// ---------------------------------------------------------------------------
// One run
// ---------------------------------------------------------------------------

struct Row {
  const char* name = "";
  double throughput_mops = 0.0;
  std::uint64_t p50 = 0, p99 = 0, p999 = 0, max = 0;
  int producer_cpu = -1, consumer_cpu = -1;
  std::size_t processed = 0;
};

static std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
                                double p) {
  return sorted[static_cast<std::size_t>(
      p / 100.0 * static_cast<double>(sorted.size() - 1))];
}

static Order makeOrder(std::uint64_t id) noexcept {
  Order o{};
  o.id = id;
  o.side = (id & 1u) ? Side::Buy : Side::Sell;
  o.price = 10000 + static_cast<Price>(id % 100);
  o.quantity = 1 + (id % 100);
  return o;
}

template <class Buffer>
static Row runLayout(const char* name, std::size_t total, int cpu_producer,
                     int cpu_consumer) {
  auto buf = std::make_unique<Buffer>();

  // Discard this many orders before recording.  The default run uses 10000;
  // tiny runs scale it down so they still have measured samples.
  const std::size_t warmup = std::min<std::size_t>(10000, total / 10);

  std::vector<std::uint64_t> lat(total);

  std::atomic<bool> go{false};
  int producer_cpu = -1;
  int consumer_cpu = -1;
  std::size_t pushed = 0;
  std::size_t processed = 0;
  Clock::time_point t0{}, t1{};

  std::thread producer([&] {
    pinThisThread(cpu_producer);
    producer_cpu = currentCpu();
    while (!go.load(std::memory_order_acquire)) cpuRelax();

    for (std::size_t i = 0; i < warmup; ++i) {
      Order o = makeOrder(i);
      o.submit_time = Clock::now();
      buf->push(o);
      ++pushed;
    }
    for (std::size_t i = 0; i < total; ++i) {
      Order o = makeOrder(warmup + i);
      o.submit_time = Clock::now();
      buf->push(o);
      ++pushed;
    }
  });

  std::thread consumer([&] {
    pinThisThread(cpu_consumer);
    consumer_cpu = currentCpu();
    while (!go.load(std::memory_order_acquire)) cpuRelax();

    for (std::size_t i = 0; i < warmup; ++i) {
      (void)buf->pop();
    }

    t0 = Clock::now();
    for (std::size_t i = 0; i < total; ++i) {
      const Order o = buf->pop();
      lat[i] = nowNs() - nsSinceEpoch(o.submit_time);
      ++processed;
    }
    t1 = Clock::now();
  });

  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  std::sort(lat.begin(), lat.end());

  Row row;
  row.name = name;
  row.throughput_mops =
      seconds > 0.0 ? static_cast<double>(total) / seconds / 1e6 : 0.0;
  row.p50 = percentile(lat, 50.0);
  row.p99 = percentile(lat, 99.0);
  row.p999 = percentile(lat, 99.9);
  row.max = lat.back();
  row.producer_cpu = producer_cpu;
  row.consumer_cpu = consumer_cpu;
  row.processed = processed;

  if (pushed != total + warmup) {
    std::fprintf(stderr, "FATAL: producer pushed %zu of %zu orders\n", pushed,
                 total + warmup);
    std::exit(1);
  }
  return row;
}

static void printRow(const Row& r) {
  std::printf("%-14s %8.2f M/s %8llu %8llu %8llu %10llu\n", r.name,
              r.throughput_mops,
              static_cast<unsigned long long>(r.p50),
              static_cast<unsigned long long>(r.p99),
              static_cast<unsigned long long>(r.p999),
              static_cast<unsigned long long>(r.max));
  std::printf("  cores: producer=%d consumer=%d (%s)\n", r.producer_cpu,
              r.consumer_cpu, r.name);
}

int main(int argc, char** argv) {
  const std::size_t total =
      argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
  const int cpu_producer = argc > 2 ? std::atoi(argv[2]) : 6;
  const int cpu_consumer = argc > 3 ? std::atoi(argv[3]) : 2;

  if (total == 0) {
    std::fprintf(stderr, "orders must be > 0\n");
    return 1;
  }

  constexpr std::size_t kCapacity = 1u << 14;  // 16384 slots

  std::printf(
      "orders=%zu capacity=%zu requested pinning: producer=%d consumer=%d "
      "(-1 = none)\n",
      total, kCapacity, cpu_producer, cpu_consumer);
  std::printf(
      "\nlayout          throughput        p50      p99     p99.9        max"
      "      (latency ns, includes ~25 ns clock cost)\n");

  const Row padded =
      runLayout<SpscPadded<kCapacity>>("padded", total, cpu_producer,
                                       cpu_consumer);
  printRow(padded);

  const Row packed =
      runLayout<SpscPacked<kCapacity>>("packed", total, cpu_producer,
                                       cpu_consumer);
  printRow(packed);

  const double ratio = packed.throughput_mops > 0.0
                           ? padded.throughput_mops / packed.throughput_mops
                           : 0.0;
  std::printf("\npadded_vs_packed_throughput=%.2f x\n", ratio);

  const bool ok = padded.processed == total && packed.processed == total;
  return ok ? 0 : 1;
}
