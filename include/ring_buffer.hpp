#pragma once

#include "../include/order.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

class RingBuffer {

public:
  // Same capacity as the SPSC queue used in the comparison.  The object is
  // heap-allocated by the application, so this is ~768 KB, not a stack array.
  static constexpr std::size_t CAPACITY = 16384;

private:
  std::array<Order, CAPACITY> buffer{};

  // insert at tail and pop from head
  // maintain a gap of 1

  std::size_t tail = 0;
  std::size_t head = 0;
  std::size_t count = 0;

  mutable std::mutex mtx;
  std::condition_variable not_empty;
  std::condition_variable not_full;

  // Number of pushes/pops that found the buffer full/empty and had to sleep on
  // a condition variable.  Relaxed because they are read only after the
  // threads join; the point is the count, not synchronisation.
  std::atomic<std::uint64_t> waits_full_{0};
  std::atomic<std::uint64_t> waits_empty_{0};

public:
  RingBuffer() = default;

  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;

  // Same names as SpscRingBuffer so the driver can report either queue.
  std::uint64_t fullWaits() const noexcept {
    return waits_full_.load(std::memory_order_relaxed);
  }
  std::uint64_t emptyWaits() const noexcept {
    return waits_empty_.load(std::memory_order_relaxed);
  }

  // push the order at the back
  // waits if the buffer is full
  void push(const Order &ord);

  // pops the order from the front and return Order obj
  // waits if the buffer is empty.
  Order pop();

  // check whether the buffer is empty.
  bool empty() const;

  // check whether buffer is full.
  bool full() const;
};
