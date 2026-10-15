#pragma once

#include "../include/order.hpp"
#include "../include/slab_allocator.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

class RingBuffer {

private:
  static constexpr std::size_t CAPACITY = 128;

  std::array<Order, CAPACITY> buffer{};

  // insert at tail and pop from head
  // maintain a gap of 1

  std::size_t tail = 1;
  std::size_t head = 0;
  std::size_t count = 0;

  mutable std::mutex mtx;

public:
  RingBuffer() = default;

  RingBuffer(const RingBuffer &) = delete;
  RingBuffer(operator=(cosnt RingBuffer&) = delete;

  // push the order at the back
  // return false if the buffer is full
  bool push(const Order& ord);

  // pop the order from the front
  // returns std::nullopt if the buffer is empty.
  std::optional<Order> pop();

  // check whether the buffer is empty.
  bool empty() const;

  // check whether buffer is full.
  bool full() const;
};
