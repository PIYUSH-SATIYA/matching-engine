#include "ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "fail: " << message << '\n';
    ++failures;
  }
}

Order make_order(OrderId id) {
  return Order{id, (id % 2 == 0) ? Side::Buy : Side::Sell,
               static_cast<Price>(100000 + id),
               static_cast<Quantity>(id % 100 + 1),
               Timestamp{} + std::chrono::nanoseconds(id)};
}

bool same_order(const Order &a, const Order &b) {
  return a.id == b.id && a.side == b.side && a.price == b.price &&
         a.quantity == b.quantity && a.submit_time == b.submit_time;
}

void test_initial_state() {
  RingBuffer queue;

  check(queue.empty(), "new queue should be empty");
  check(!queue.full(), "new queue should not be full");
}

void test_single_order() {
  RingBuffer queue;
  const Order expected = make_order(42);

  queue.push(expected);
  check(!queue.empty(), "queue should not be empty after push");

  const Order actual = queue.pop();
  check(same_order(expected, actual),
        "popped order should match all fields of pushed order");

  check(queue.empty(), "queue should be empty after pop");
}

void test_fifo() {
  RingBuffer queue;

  for (OrderId id = 1; id <= 10; id++) {
    queue.push(make_order(id));
  }

  for (OrderId id = 1; id <= 10; ++id) {
    const Order actual = queue.pop();
    check(same_order(make_order(id), actual),
          "FIFO order or order fields are incorrect");
  }

  check(queue.empty(), "queue should be empty after FIFO test");
}

void test_capacity() {
  RingBuffer queue;

  for (OrderId id = 1; id <= 128; ++id) {
    queue.push(make_order(id));
  }

  check(queue.full(), "queue should be full at capacity 128");

  // Free one slot, then reuse it.
  const Order first = queue.pop();
  check(first.id == 1, "first order should be removed first");
  check(!queue.full(), "queue should have space after one pop");

  queue.push(make_order(129));
  check(queue.full(), "queue should become full again");

  for (OrderId id = 2; id <= 129; ++id) {
    const Order actual = queue.pop();
    check(actual.id == id, "capacity test FIFO order incorrect");
  }

  check(queue.empty(), "queue should be empty after draining");
}

void test_wraparound() {
  RingBuffer queue;

  // Force head and tail to wrap around the array repeatedly.
  for (OrderId round = 0; round < 10; ++round) {
    for (OrderId i = 0; i < 100; ++i) {
      queue.push(make_order(round * 100 + i + 1));
    }

    for (OrderId i = 0; i < 100; ++i) {
      const Order expected = make_order(round * 100 + i + 1);
      const Order actual = queue.pop();

      check(same_order(expected, actual), "wraparound corrupted FIFO order");
    }
  }

  check(queue.empty(), "queue should be empty after wraparound test");
}

void test_consumer_blocks_when_empty() {
  RingBuffer queue;
  std::atomic<bool> completed{false};
  Order received{};

  std::thread consumer([&] {
    received = queue.pop();
    completed.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(50ms);

  const bool blocked = !completed.load(std::memory_order_acquire);

  // Always unblock and join before checking the result.
  const Order expected = make_order(500);
  queue.push(expected);
  consumer.join();

  check(blocked, "consumer should wait when queue is empty");
  check(same_order(expected, received),
        "consumer should receive the inserted order");
}

void test_producer_blocks_when_full() {
  RingBuffer queue;

  for (OrderId id = 1; id <= 128; ++id) {
    queue.push(make_order(id));
  }

  std::atomic<bool> completed{false};

  std::thread producer([&] {
    queue.push(make_order(129));
    completed.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(50ms);

  const bool blocked = !completed.load(std::memory_order_acquire);

  // Free one slot so the producer can finish.
  const Order first = queue.pop();
  producer.join();

  check(blocked, "producer should wait when queue is full");
  check(first.id == 1, "first order should be removed to free space");
  check(completed.load(std::memory_order_acquire),
        "producer should finish after space becomes available");

  for (OrderId id = 2; id <= 129; ++id) {
    check(queue.pop().id == id, "producer blocking test violated FIFO order");
  }

  check(queue.empty(), "queue should be empty after draining");
}

void test_concurrent_fifo_stress() {
  RingBuffer queue;
  constexpr OrderId TOTAL = 200000;

  std::atomic<bool> sequence_ok{true};

  std::thread producer([&] {
    for (OrderId id = 1; id <= TOTAL; ++id) {
      queue.push(make_order(id));
    }
  });

  std::thread consumer([&] {
    for (OrderId expected_id = 1; expected_id <= TOTAL; ++expected_id) {

      const Order actual = queue.pop();

      if (!same_order(make_order(expected_id), actual)) {
        sequence_ok.store(false, std::memory_order_relaxed);
      }
    }
  });

  producer.join();
  consumer.join();

  check(sequence_ok.load(std::memory_order_relaxed),
        "concurrent stress: lost, duplicated, reordered, "
        "or corrupted order");

  check(queue.empty(), "stress test should leave queue empty");
}

} // namespace

int main() {
  test_initial_state();
  test_single_order();
  test_fifo();
  test_capacity();
  test_wraparound();
  test_consumer_blocks_when_empty();
  test_producer_blocks_when_full();
  test_concurrent_fifo_stress();

  if (failures != 0) {
    std::cerr << failures << " test check(s) failed.\n";
    return 1;
  }

  std::cout << "All ring buffer tests passed.\n";
  return 0;
}
