// Tests for SpscRingBuffer. Run it also under ThreadSanitizer:
//   cmake -DENABLE_TSAN=ON ..   (the cross-thread test is the important one)

#include <cstdio>
#include <cstdlib>
#include <thread>

#include "spsc_ring_buffer.hpp"

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

static Order mk(OrderId id) {
  Order o{};
  o.id = id;
  o.side = (id & 1) ? Side::Buy : Side::Sell;
  o.price = static_cast<Price>(id * 7);
  o.quantity = id * 3;
  return o;
}

static void singleThreaded() {
  SpscRingBuffer<8> q;
  Order o{};
  CHECK(q.empty() && !q.full());
  CHECK(!q.try_pop(o));                       // pop from empty fails
  for (OrderId i = 1; i <= 8; ++i) CHECK(q.try_push(mk(i)));  // all 8 slots usable
  CHECK(q.full() && !q.try_push(mk(99)));     // push to full fails
  for (OrderId i = 1; i <= 8; ++i) {          // FIFO order
    CHECK(q.try_pop(o) && o.id == i);
  }
  CHECK(q.empty());
  // wrap-around many times
  for (OrderId i = 1; i <= 1000; ++i) {
    CHECK(q.try_push(mk(i)));
    CHECK(q.try_pop(o) && o.id == i && o.quantity == i * 3);
  }
  std::puts("  single-thread semantics OK (full/empty/FIFO/wrap)");
}

static void twoThreads(std::size_t n) {
  SpscRingBuffer<1024> q;
  std::thread producer([&] {
    for (OrderId i = 1; i <= n; ++i) q.push(mk(i));
  });
  bool ok = true;
  for (OrderId i = 1; i <= n; ++i) {
    Order o = q.pop();
    // every field must be intact and in order (no loss, no duplicate, no tearing)
    if (o.id != i || o.quantity != i * 3 || o.price != static_cast<Price>(i * 7) ||
        o.side != ((i & 1) ? Side::Buy : Side::Sell)) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    // unblock the producer so the thread can be joined before failing
    Order drain;
    while (q.try_pop(drain)) {}
  }
  producer.join();
  CHECK(ok);
  CHECK(q.empty());
  std::printf("  two-thread transfer OK (%zu orders, in order, no loss)\n", n);
}

int main() {
  std::puts("SpscRingBuffer:");
  singleThreaded();
  twoThreads(3'000'000);
  std::puts("All SPSC ring buffer tests passed.");
  return 0;
}
