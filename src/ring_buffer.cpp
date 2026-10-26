#include "../include/ring_buffer.hpp"

#include <mutex>
#include <utility>

void RingBuffer::push(const Order &ord) {
  std::unique_lock<std::mutex> lock(mtx);

  // sleep while the buffer is full
  not_full.wait(lock, [this] { return count < CAPACITY; });

  buffer[tail] = ord;
  tail = (tail + 1) % CAPACITY;
  count++;

  // relase the mutex before notifying the consumer
  lock.unlock();

  not_empty.notify_one();
}

Order RingBuffer::pop() {
  std::unique_lock<std::mutex> lock(mtx);

  // sleep while the buffer is empty.
  not_empty.wait(lock, [this] { return count > 0; });

  Order ord = buffer[head];

  head = (head + 1) % CAPACITY;

  count--;

  // relase the lock before notifying the producer.
  lock.unlock();

  not_full.notify_one();

  return ord;
}

bool RingBuffer::empty() const {
  std::lock_guard<std::mutex> lock(mtx);
  return count == 0;
}

bool RingBuffer::full() const {
  std::lock_guard<std::mutex> lock(mtx);
  return count == CAPACITY;
}
