#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

// Preallocated latency recorder.  The first `warmup` recordings are ignored
// (the measurement protocol discards the first 10,000 orders).  Recording is
// O(1); sorting happens only when the run ends.
class LatencyRecorder {
 public:
    explicit LatencyRecorder(std::size_t capacity, std::size_t warmup = 0)
        : capacity_(capacity), warmup_(warmup) {
        samples_.reserve(capacity);
    }

    void record(std::uint64_t ns) noexcept {
        if (seen_ < warmup_) {
            ++seen_;
            return;
        }
        ++seen_;
        if (samples_.size() < capacity_) {
            samples_.push_back(ns);
        } else {
            ++dropped_;
        }
    }

    std::size_t count() const noexcept { return samples_.size(); }
    std::size_t seen() const noexcept { return seen_; }
    std::size_t dropped() const noexcept { return dropped_; }
    std::size_t warmup() const noexcept { return warmup_; }

    std::vector<std::uint64_t> sorted() const {
        std::vector<std::uint64_t> v = samples_;
        std::sort(v.begin(), v.end());
        return v;
    }

    static std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
                                    double p) {
        if (sorted.empty()) return 0;
        const std::size_t idx =
            static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
    }

    std::uint64_t max() const {
        if (samples_.empty()) return 0;
        return *std::max_element(samples_.begin(), samples_.end());
    }

    void summary() const {
        if (samples_.empty()) {
            std::printf("LatencyRecorder: no samples (seen=%zu warmup=%zu)\n",
                        seen_, warmup_);
            return;
        }
        const auto v = sorted();
        std::printf(
            "count=%-10zu p50=%-10llu p90=%-10llu p99=%-10llu p99.9=%-10llu "
            "max=%-10llu ns\n",
            v.size(), static_cast<unsigned long long>(percentile(v, 0.50)),
            static_cast<unsigned long long>(percentile(v, 0.90)),
            static_cast<unsigned long long>(percentile(v, 0.99)),
            static_cast<unsigned long long>(percentile(v, 0.999)),
            static_cast<unsigned long long>(v.back()));
    }

    // Binary format: u64 count followed by `count` u64 latency samples.
    bool writeSamples(const char* path) const {
        if (!path) return false;
        FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        const std::uint64_t n = samples_.size();
        std::fwrite(&n, sizeof(n), 1, f);
        if (n) std::fwrite(samples_.data(), sizeof(std::uint64_t), n, f);
        std::fclose(f);
        return true;
    }

 private:
    std::vector<std::uint64_t> samples_;
    std::size_t capacity_{0};
    std::size_t warmup_{0};
    std::size_t seen_{0};
    std::size_t dropped_{0};
};
