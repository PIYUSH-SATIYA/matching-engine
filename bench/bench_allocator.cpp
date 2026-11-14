// bench_allocator.cpp — isolate allocator cost, which the integrated
// benchmarks cannot.
//
// Two variants allocate/free the same 64-byte node under the same deterministic
// workload plan:
//
//   (A) general heap : aligned 64-byte blocks from ::operator new / delete
//   (B) slab         : SlabAllocator<Node64, 65536> from include/slab_allocator.hpp
//
// Workload: keep a live working set of K nodes (default 8192).  Each of N
// operations is either
//   70% : allocate if the working set is below K, otherwise "reuse" (touch a
//         live node without asking the allocator for more memory), or
//   30% : release a uniformly chosen live node.
// The live set is pre-filled to K/2 before measurement, so it converges to ~K.
// It can never exceed K, which is also the slab capacity: if the slab ever
// reports exhaustion the workload logic is wrong, so we exit(1).
//
// TIMING CAVEAT: every timed operation is bracketed by two steady_clock reads.
// On this class of machine that is roughly 20-25 ns of clock cost, and nothing
// is subtracted.  Absolute ns/op and percentiles are therefore inflated.  Both
// variants pay the *same* clock cost, so the comparison/speedup is valid.
//
// Build:
//   g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wformat=2 -Iinclude -Isrc
//       bench/bench_allocator.cpp -o bench_allocator -pthread
// Run:
//   ./bench_allocator [cpu=2] [ops=2000000] [live_set=8192] [seed=42]
//   cpu < 0 disables pinning.  The default is CPU 2, matching the other benches.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "affinity.hpp"
#include "slab_allocator.hpp"

using Clock = std::chrono::steady_clock;

static std::uint64_t nowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

// Exactly one cache line, exactly 64 bytes.
struct alignas(64) Node64 {
  std::uint64_t w[7];
  std::int32_t x;
  std::int32_t y;
};
static_assert(sizeof(Node64) == 64, "Node64 must be exactly 64 bytes");
static_assert(alignof(Node64) == 64, "Node64 must be cache-line aligned");

// ---------------------------------------------------------------------------
// Deterministic workload plan
// ---------------------------------------------------------------------------

enum class StepKind : std::uint8_t { Allocate, Release, Rest };

struct Step {
  StepKind kind;
  // For Release: index into the live-pointer vector at that point in the run.
  std::uint32_t pos;
};

// Simulate the live-set size so that every Release step carries a position that
// is valid when the same plan is replayed for either allocator.
static std::vector<Step> makePlan(std::size_t n, std::size_t live_set,
                                  std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Step> plan;
  plan.reserve(n);

  // The runner pre-fills this many nodes before measurement.
  std::size_t live = live_set / 2;

  for (std::size_t i = 0; i < n; ++i) {
    if (rng() % 100 < 70) {
      if (live < live_set) {
        plan.push_back({StepKind::Allocate, 0});
        ++live;
      } else {
        // Working set is full: reuse a live node, do not allocate.
        plan.push_back({StepKind::Rest, 0});
      }
    } else if (live == 0) {
      // Nothing to release (can only happen very early): rest.
      plan.push_back({StepKind::Rest, 0});
    } else {
      plan.push_back(
          {StepKind::Release, static_cast<std::uint32_t>(rng() % live)});
      --live;
    }
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Per-op statistics
// ---------------------------------------------------------------------------

struct Stats {
  double ns_per_op = 0.0;
  std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
  double sd = 0.0;
  std::size_t n = 0;
};

static std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
                                double p) {
  return sorted[static_cast<std::size_t>(
      p / 100.0 * static_cast<double>(sorted.size() - 1))];
}

// `v` is sorted in place.  `total_ns` is the accumulated sum of the same
// samples (kept while measuring, so the mean is consistent with ns/op).
static Stats summarize(std::vector<std::uint64_t>& v, std::uint64_t total_ns) {
  Stats s;
  s.n = v.size();
  if (v.empty()) return s;

  s.ns_per_op = static_cast<double>(total_ns) / static_cast<double>(v.size());

  std::sort(v.begin(), v.end());
  s.p50 = percentile(v, 50.0);
  s.p90 = percentile(v, 90.0);
  s.p99 = percentile(v, 99.0);
  s.p999 = percentile(v, 99.9);
  s.max = v.back();

  const long double mean =
      static_cast<long double>(total_ns) / static_cast<long double>(v.size());
  long double acc = 0.0L;
  for (std::uint64_t x : v) {
    const long double d = static_cast<long double>(x) - mean;
    acc += d * d;
  }
  s.sd = std::sqrt(static_cast<double>(
      acc / static_cast<long double>(v.size())));
  return s;
}

// ---------------------------------------------------------------------------
// Allocator adapters
// ---------------------------------------------------------------------------

struct HeapAllocator {
  Node64* allocate() {
    Node64* p = static_cast<Node64*>(
        ::operator new(sizeof(Node64), std::align_val_t{alignof(Node64)}));
    ++live_;
    return p;
  }

  void deallocate(Node64* p) noexcept {
    ::operator delete(p, std::align_val_t{alignof(Node64)});
    --live_;
  }

  std::size_t live_now() const noexcept { return live_; }

 private:
  std::size_t live_ = 0;
};

static constexpr std::size_t kSlabCapacity = 1u << 16;  // 65536 nodes = 4 MiB

struct SlabAllocatorWrapper {
  Node64* allocate() noexcept { return slab_.allocate(); }

  void deallocate(Node64* p) noexcept {
    if (!slab_.deallocate(p)) {
      std::fprintf(stderr, "FATAL: slab rejected a pointer it handed out\n");
      std::exit(1);
    }
  }

  std::size_t live_now() const noexcept { return slab_.allocated(); }

 private:
  SlabAllocator<Node64, kSlabCapacity> slab_;
};

struct Result {
  Stats alloc;
  Stats free;
  std::size_t live_end = 0;
  std::uint64_t alloc_ops = 0;
  std::uint64_t free_ops = 0;
};

// ---------------------------------------------------------------------------
// Workload replay
// ---------------------------------------------------------------------------

template <class Allocator>
static Result run(const std::vector<Step>& plan, std::size_t live_set,
                  Allocator& a) {
  std::vector<Node64*> live;
  live.reserve(live_set);

  // Pre-fill outside measurement, starting every node's lifetime.
  for (std::size_t i = 0; i < live_set / 2; ++i) {
    Node64* p = a.allocate();
    if (p == nullptr) {
      std::fprintf(stderr, "FATAL: allocator exhausted during pre-fill\n");
      std::exit(1);
    }
    live.push_back(::new (static_cast<void*>(p)) Node64);
  }

  // Untimed warm-up: prime allocator caches without changing the live set.
  for (int i = 0; i < 1024; ++i) {
    Node64* p = a.allocate();
    if (p == nullptr) {
      std::fprintf(stderr, "FATAL: allocator exhausted during warm-up\n");
      std::exit(1);
    }
    a.deallocate(p);
  }

  std::vector<std::uint64_t> lat_alloc;
  std::vector<std::uint64_t> lat_free;
  lat_alloc.reserve(plan.size());
  lat_free.reserve(plan.size());

  std::uint64_t sum_alloc = 0, sum_free = 0;
  std::uint64_t n_alloc = 0, n_free = 0;

  for (const Step& s : plan) {
    switch (s.kind) {
      case StepKind::Allocate: {
        const std::uint64_t t0 = nowNs();
        Node64* p = a.allocate();
        const std::uint64_t t1 = nowNs();

        if (p == nullptr) {
          std::fprintf(stderr,
                       "FATAL: allocator exhausted — workload logic is wrong\n");
          std::exit(1);
        }

        // Begin the node's lifetime outside the timed window (trivial type, so
        // this compiles to nothing) and touch it like a real book node.
        p = ::new (static_cast<void*>(p)) Node64;
        p->w[0] = n_alloc;

        live.push_back(p);
        lat_alloc.push_back(t1 - t0);
        sum_alloc += t1 - t0;
        ++n_alloc;
        break;
      }

      case StepKind::Release: {
        Node64* p = live[s.pos];
        live[s.pos] = live.back();
        live.pop_back();

        const std::uint64_t t0 = nowNs();
        a.deallocate(p);
        const std::uint64_t t1 = nowNs();

        lat_free.push_back(t1 - t0);
        sum_free += t1 - t0;
        ++n_free;
        break;
      }

      case StepKind::Rest: {
        // "Reuse": touch, but do not ask the allocator for a node.
        if (!live.empty()) {
          live[n_alloc % live.size()]->w[1] += 1;
        }
        break;
      }
    }
  }

  Result r;
  r.live_end = live.size();
  if (a.live_now() != r.live_end) {
    std::fprintf(stderr,
                 "FATAL: allocator live count %zu != tracked live count %zu\n",
                 a.live_now(), r.live_end);
    std::exit(1);
  }
  r.alloc_ops = n_alloc;
  r.free_ops = n_free;
  r.alloc = summarize(lat_alloc, sum_alloc);
  r.free = summarize(lat_free, sum_free);

  // Untimed drain: every remaining node is released, so nothing leaks.
  for (Node64* p : live) a.deallocate(p);
  return r;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void printRow(const char* name, const Result& r) {
  std::printf(
      "%-26s %10.2f | %8llu %6llu %6llu %7llu %8llu | %8.2f | "
      "%7llu %6llu %6llu %7llu %8llu | %8.2f %8.2f\n",
      name, r.alloc.ns_per_op,
      static_cast<unsigned long long>(r.alloc.p50),
      static_cast<unsigned long long>(r.alloc.p90),
      static_cast<unsigned long long>(r.alloc.p99),
      static_cast<unsigned long long>(r.alloc.p999),
      static_cast<unsigned long long>(r.alloc.max), r.free.ns_per_op,
      static_cast<unsigned long long>(r.free.p50),
      static_cast<unsigned long long>(r.free.p90),
      static_cast<unsigned long long>(r.free.p99),
      static_cast<unsigned long long>(r.free.p999),
      static_cast<unsigned long long>(r.free.max), r.alloc.sd, r.free.sd);
}

int main(int argc, char** argv) {
  const int cpu = argc > 1 ? std::atoi(argv[1]) : 2;
  const std::size_t ops =
      argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000000;
  const std::size_t live_set =
      argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 8192;
  const std::uint64_t seed =
      argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 42;

  if (live_set == 0 || live_set > kSlabCapacity) {
    std::fprintf(stderr, "live_set must be in [1, %zu]\n", kSlabCapacity);
    return 1;
  }

  pinThisThread(cpu);

  std::printf("ops=%zu live_set=%zu seed=%llu cpu=%d\n", ops, live_set,
              static_cast<unsigned long long>(seed), cpu);

  const std::vector<Step> plan = makePlan(ops, live_set, seed);

  std::printf(
      "%-26s %10s | %8s %6s %6s %7s %8s | %8s | "
      "%7s %6s %6s %7s %8s | %8s %8s\n",
      "allocator", "alloc ns/op", "alloc p50", "p90", "p99", "p99.9",
      "max  (ns)", "free ns/op", "free p50", "p90", "p99", "p99.9",
      "max  (ns)", "alloc sd", "free sd");

  auto heap = std::make_unique<HeapAllocator>();
  const Result hr = run(plan, live_set, *heap);
  printRow("heap ::operator new/delete", hr);

  auto slab = std::make_unique<SlabAllocatorWrapper>();
  const Result sr = run(plan, live_set, *slab);
  printRow("slab (SlabAllocator)", sr);

  const bool live_ok = hr.live_end == sr.live_end;
  const bool ops_ok =
      hr.alloc_ops == sr.alloc_ops && hr.free_ops == sr.free_ops;
  std::printf("live_ok=%s ops_ok=%s\n", live_ok ? "YES" : "NO",
              ops_ok ? "YES" : "NO");

  const double alloc_speedup =
      sr.alloc.ns_per_op > 0.0 ? hr.alloc.ns_per_op / sr.alloc.ns_per_op : 0.0;
  const double free_speedup =
      sr.free.ns_per_op > 0.0 ? hr.free.ns_per_op / sr.free.ns_per_op : 0.0;
  std::printf(
      "slab_vs_heap_speedup alloc=%.2f x free=%.2f x  (heap_ns / slab_ns)\n",
      alloc_speedup, free_speedup);

  return (live_ok && ops_ok) ? 0 : 1;
}
