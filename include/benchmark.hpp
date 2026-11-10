#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

class LatencyRecorder {
 public:
    explicit LatencyRecorder(std::size_t capacity) {
        samples_.reserve(capacity);
        capacity_ = capacity;
    }

    void record(std::uint64_t ns) noexcept {
        if (samples_.size() < capacity_) {
            samples_.push_back(ns);
        }
    }

    void summary() const {
        if (samples_.empty()) {
            std::printf("LatencyRecorder: no samples\n");
            return;
        }
        std::vector<std::uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t n = sorted.size();
        auto pct = [&](double p) -> std::uint64_t {
            std::size_t idx = static_cast<std::size_t>(p * (n - 1));
            return sorted[idx];
        };
        std::printf("count=%-10zu p50=%-10llu p90=%-10llu p99=%-10llu p99.9=%-10llu max=%-10llu ns\n",
                    n,
                    (unsigned long long)pct(0.50),
                    (unsigned long long)pct(0.90),
                    (unsigned long long)pct(0.99),
                    (unsigned long long)pct(0.999),
                    (unsigned long long)sorted.back());
    }

    std::size_t count() const noexcept { return samples_.size(); }

 private:
    std::vector<std::uint64_t> samples_;
    std::size_t capacity_{0};
};
