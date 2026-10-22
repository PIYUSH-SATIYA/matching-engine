#pragma once

#include "../include/order.hpp"
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>

class RingBuffer {

private:
  static constexpr std::size_t CAPACITY = 128;

  std::array<Order, CAPACITY> buffer{};

  // insert at tail and pop from head
  // maintain a gap of 1

  std::size_t tail = 0;
  std::size_t head = 0;
  std::size_t count = 0;

  mutable std::mutex mtx;
  std::condition_variable not_empty;
  std::condition_variable not_full;

public:
  RingBuffer() = default;

  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;

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
