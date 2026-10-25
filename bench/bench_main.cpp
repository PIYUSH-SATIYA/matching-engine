// Batched SlabAllocator<Order> versus new/delete microbenchmark.
// Each measured batch times the same work for both allocators: allocate and
// construct BATCH_SIZE Orders, then destroy and deallocate them. Batch timing
// amortizes clock-call overhead; each reported sample is a batch average, not
// an individual operation latency. Implementations alternate in randomized
// order by round to reduce fixed-order bias. Results are local observations,
// not a substitute for repeated controlled benchmark runs.

#include "slab_allocator.hpp"
#include "order.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

static constexpr std::size_t WARMUP_OPS = 10'000;
static constexpr std::size_t OPS_PER_ROUND = 9'984; // 156 complete batches
static constexpr std::size_t ROUNDS = 100;
static constexpr std::size_t BATCH_SIZE = 64;
static constexpr std::size_t SLAB_CAP = BATCH_SIZE;
static constexpr std::size_t BATCHES_PER_ROUND = OPS_PER_ROUND / BATCH_SIZE;
static volatile std::uint64_t checksum_sink = 0;

struct Stats { double p50, p99, p999, max, stddev; };

static Stats compute_stats(std::vector<long long>& samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    auto percentile = [&](double p) {
        const double index = p * static_cast<double>(n - 1);
        const auto lo = static_cast<std::size_t>(index);
        const auto hi = std::min(lo + 1, n - 1);
        return samples[lo] + (index - lo) * (samples[hi] - samples[lo]);
    };
    const double mean = static_cast<double>(
        std::accumulate(samples.begin(), samples.end(), 0LL)) / n;
    double squares = 0.0;
    for (long long sample : samples) {
        const double delta = sample - mean;
        squares += delta * delta;
    }
    return {percentile(.5), percentile(.99), percentile(.999),
            static_cast<double>(samples.back()), std::sqrt(squares / n)};
}

static void print_stats(const char* name, const Stats& stats) {
    std::printf("  %-24s p50=%7.1f ns p99=%7.1f p99.9=%7.1f max=%8.1f stddev=%7.1f\n",
        name, stats.p50, stats.p99, stats.p999, stats.max, stats.stddev);
}

static long long ticks() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename Allocate, typename Release>
static void run_batches(std::vector<long long>& alloc_samples,
                        std::vector<long long>& free_samples,
                        Allocate allocate, Release release) {
    std::array<Order*, BATCH_SIZE> live{};
    for (std::size_t batch = 0; batch < BATCHES_PER_ROUND; ++batch) {
        constexpr std::size_t count = BATCH_SIZE;
        long long begin = ticks();
        for (std::size_t i = 0; i < count; ++i) {
            const OrderId id = static_cast<OrderId>(batch * BATCH_SIZE + i);
            live[i] = allocate(id);
            if (!live[i]) std::abort();
        }
        alloc_samples.push_back(ticks() - begin);

        std::uint64_t checksum = checksum_sink;
        for (std::size_t i = 0; i < count; ++i) checksum += live[i]->id;
        checksum_sink = checksum;

        begin = ticks();
        for (std::size_t i = count; i > 0; --i) release(live[i - 1]);
        free_samples.push_back(ticks() - begin);
    }
}

static void warmup_slab() {
    SlabAllocator<Order, SLAB_CAP> slab;
    for (std::size_t i = 0; i < WARMUP_OPS; ++i) {
        Order* p = slab.allocate();
        new (p) Order{static_cast<OrderId>(i), Side::Buy, 100, 1, {}};
        p->~Order();
        (void)slab.deallocate(p);
    }
}

static void warmup_new() {
    for (std::size_t i = 0; i < WARMUP_OPS; ++i) {
        Order* p = new Order{static_cast<OrderId>(i), Side::Buy, 100, 1, {}};
        delete p;
    }
}

int main() {
    std::vector<long long> slab_alloc, slab_free, new_alloc, new_free;
    slab_alloc.reserve(ROUNDS * BATCHES_PER_ROUND);
    slab_free.reserve(ROUNDS * BATCHES_PER_ROUND);
    new_alloc.reserve(ROUNDS * BATCHES_PER_ROUND);
    new_free.reserve(ROUNDS * BATCHES_PER_ROUND);
    std::mt19937 rng(0x5A17u);

    // Warm up both paths before collecting samples.
    warmup_slab();
    warmup_new();

    for (std::size_t round = 0; round < ROUNDS; ++round) {
        const bool slab_first = (rng() & 1u) != 0;
        auto slab_run = [&] {
            SlabAllocator<Order, SLAB_CAP> slab;
            run_batches(slab_alloc, slab_free,
                [&](OrderId id) {
                    Order* p = slab.allocate();
                    if (p) new (p) Order{id, Side::Buy, 100, 1, {}};
                    return p;
                },
                [&](Order* p) { p->~Order(); (void)slab.deallocate(p); });
        };
        auto new_run = [&] {
            run_batches(new_alloc, new_free,
                [](OrderId id) { return new Order{id, Side::Buy, 100, 1, {}}; },
                [](Order* p) { delete p; });
        };
        if (slab_first) { slab_run(); new_run(); }
        else { new_run(); slab_run(); }
    }

    Stats sa = compute_stats(slab_alloc), sf = compute_stats(slab_free);
    Stats na = compute_stats(new_alloc), nf = compute_stats(new_free);
    std::printf("Batched allocator benchmark: %zu rounds, %zu operations/round, "
                "%zu live max, %zu warm-up operations per path.\n",
                ROUNDS, OPS_PER_ROUND, BATCH_SIZE, WARMUP_OPS);
    std::printf("Samples are elapsed ns per %zu-operation batch (%zu samples/path); alloc includes "
                "construction, free includes destruction.\n",
                BATCH_SIZE, slab_alloc.size());
    std::printf("Implementations run in randomized order each round; run repeatedly "
                "on an otherwise idle machine for comparisons.\n\n");
    print_stats("Slab allocate+construct", sa);
    print_stats("new (allocate+construct)", na);
    print_stats("Slab destroy+deallocate", sf);
    print_stats("delete (destroy+free)", nf);
}
