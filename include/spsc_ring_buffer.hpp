#pragma once

// SPSC lock-free ring buffer (Single Producer, Single Consumer).
//
// Same external behaviour as the mutex RingBuffer (contract section 7):
//     void  push(const Order&)   blocks (spins) while full
//     Order pop()                blocks (spins) while empty
// plus non-blocking try_push / try_pop.
//
// ASSUMES EXACTLY ONE producer thread and ONE consumer thread. Do not call
// push() from two threads. That is the whole reason it needs no lock.
//
// Why it is fast:
//  1. head_ is written ONLY by the consumer, tail_ ONLY by the producer.
//     Nobody ever races on the same variable, so no lock/CAS is needed.
//  2. head_ and tail_ live on separate cache lines (alignas(64)), otherwise
//     the two cores would fight over one line ("false sharing").
//  3. Each side keeps a LOCAL cached copy of the other side's index, so the
//     shared atomic is re-read only when the queue looks full/empty. That
//     removes most cross-core cache-line traffic.
//  4. Counters only ever increase (never wrapped), so full/empty is simply
//     tail - head; no wasted slot. Index = counter & mask (Capacity = 2^k).
//
// Memory ordering:
//  producer: write slot, then tail_.store(release)  -> "slot is ready"
//  consumer: tail_.load(acquire), then read slot    -> sees the slot data
//  consumer: read slot, then head_.store(release)   -> "slot is free again"
//  producer: head_.load(acquire), then reuse slot   -> not overwritten early

#include <atomic>
#include <cstddef>
#include <thread>

#include "order.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define ME_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__)
#define ME_CPU_RELAX() asm volatile("yield" ::: "memory")
#else
#define ME_CPU_RELAX() std::this_thread::yield()
#endif

template <std::size_t Capacity = 1024>
class SpscRingBuffer {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "SpscRingBuffer: Capacity must be a power of two");
  static constexpr std::size_t kMask = Capacity - 1;

 public:
  SpscRingBuffer() = default;
  SpscRingBuffer(const SpscRingBuffer&) = delete;
  SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

  // ---- producer thread only ------------------------------------------
  bool try_push(const Order& o) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ >= Capacity) {                       // looks full
      cached_head_ = head_.load(std::memory_order_acquire);   // refresh
      if (t - cached_head_ >= Capacity) return false;         // really full
    }
    buf_[t & kMask] = o;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  void push(const Order& o) noexcept {
    for (unsigned spins = 0; !try_push(o); ++spins) wait_hint(spins);
  }

  // ---- consumer thread only ------------------------------------------
  bool try_pop(Order& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {                                  // looks empty
      cached_tail_ = tail_.load(std::memory_order_acquire);   // refresh
      if (h == cached_tail_) return false;                    // really empty
    }
    out = buf_[h & kMask];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  Order pop() noexcept {
    Order o;
    for (unsigned spins = 0; !try_pop(o); ++spins) wait_hint(spins);
    return o;
  }

  // Approximate (racy by nature when the other thread is running).
  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }
  bool full() const noexcept {
    return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire) >= Capacity;
  }
  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  // Spin with a CPU "pause" hint; after many fruitless spins give the core
  // away so we do not starve the other thread if both share one core.
  static void wait_hint(unsigned spins) noexcept {
    if (spins < 4096) ME_CPU_RELAX();
    else std::this_thread::yield();
  }

  // consumer-owned line
  alignas(64) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};          // consumer's local copy of tail_
  // producer-owned line
  alignas(64) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};          // producer's local copy of head_
  // data
  alignas(64) Order buf_[Capacity]{};
};
