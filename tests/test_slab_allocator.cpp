#include "slab_allocator.hpp"

#include "order.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <random>
#include <set>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "fail: " << message << '\n';
    ++failures;
  }
}

Order make_order(OrderId id, Side side = Side::Buy, Price price = 100,
                 Quantity qty = 10) {
  return Order{id, side, price, qty, {}};
}

using SmallSlab = SlabAllocator<Order, 8>;

struct alignas(64) OverAligned {
  std::uint64_t value;
};

struct LifetimeTracked {
  static int constructions;
  static int destructions;

  std::uint64_t value;

  explicit LifetimeTracked(std::uint64_t v) : value(v) { ++constructions; }
  ~LifetimeTracked() { ++destructions; }
};

int LifetimeTracked::constructions = 0;
int LifetimeTracked::destructions = 0;

void test_allocate_returns_non_null() {
  SmallSlab slab;
  Order *p = slab.allocate();
  check(p != nullptr, "allocate should return a non-null pointer");
  if (p == nullptr) {
    return;
  }

  new (p) Order(make_order(1));
  check(p->id == 1, "allocated slot should be readable and writable");
  p->~Order();
  slab.deallocate(p);
}

void test_deallocate_restores_availability() {
  SmallSlab slab;
  check(slab.available() == 8, "new pool should have all slots available");

  Order *p = slab.allocate();
  check(p != nullptr, "allocate should succeed");
  if (p == nullptr) {
    return;
  }

  check(slab.available() == 7, "available should decrease after allocate");
  check(slab.allocated() == 1, "allocated should increase after allocate");

  new (p) Order(make_order(2));
  p->~Order();
  slab.deallocate(p);

  check(slab.available() == 8, "available should be restored after deallocate");
  check(slab.allocated() == 0, "allocated should be zero after deallocate");
}

void test_multiple_alloc_dealloc_cycles() {
  SmallSlab slab;

  for (int cycle = 0; cycle < 5; ++cycle) {
    Order *p = slab.allocate();
    check(p != nullptr, "allocate should succeed in every cycle");
    if (p == nullptr) {
      return;
    }

    new (p) Order(make_order(static_cast<OrderId>(cycle)));
    check(p->id == static_cast<OrderId>(cycle),
          "allocated slot should retain written data");
    p->~Order();
    slab.deallocate(p);
    check(slab.available() == 8,
          "pool should return to full capacity after each cycle");
  }
}

void test_counters_consistent_throughout_lifecycle() {
  SmallSlab slab;
  check(slab.capacity() == 8, "capacity should be 8");
  check(slab.available() == 8, "new pool should be fully available");
  check(slab.allocated() == 0, "new pool should have nothing allocated");
  check(!slab.is_full(), "new pool should not be full");

  std::array<Order *, 8> ptrs{};
  for (std::size_t i = 0; i < 8; ++i) {
    ptrs[i] = slab.allocate();
    check(ptrs[i] != nullptr, "allocate should succeed while slots remain");
    if (ptrs[i] == nullptr) {
      return;
    }

    new (ptrs[i]) Order(make_order(static_cast<OrderId>(i + 100)));
    check(slab.available() == 8 - i - 1,
          "available should track allocated slots");
    check(slab.allocated() == i + 1,
          "allocated should track allocated slots");
  }
  check(slab.is_full(), "pool should report full when all slots are used");

  for (std::size_t i = 0; i < 8; ++i) {
    ptrs[i]->~Order();
    slab.deallocate(ptrs[i]);
    check(slab.available() == i + 1,
          "available should increase after each deallocate");
    check(slab.allocated() == 8 - i - 1,
          "allocated should decrease after each deallocate");
  }
  check(!slab.is_full(), "pool should not be full after draining");
}

void test_returns_nullptr_when_full() {
  SmallSlab slab;
  std::array<Order *, 8> ptrs{};

  for (auto &p : ptrs) {
    p = slab.allocate();
    check(p != nullptr, "allocate should succeed until pool is exhausted");
    if (p == nullptr) {
      return;
    }
    new (p) Order(make_order(1));
  }
  check(slab.is_full(), "pool should be full after 8 allocations");

  Order *overflow = slab.allocate();
  check(overflow == nullptr, "allocate should return nullptr when full");

  for (auto &p : ptrs) {
    p->~Order();
    slab.deallocate(p);
  }
}

void test_recovery_after_exhaustion() {
  SmallSlab slab;
  std::array<Order *, 8> ptrs{};

  for (auto &p : ptrs) {
    p = slab.allocate();
    check(p != nullptr, "allocate should succeed while filling pool");
    if (p == nullptr) {
      return;
    }
    new (p) Order(make_order(1));
  }

  check(slab.allocate() == nullptr, "allocate should fail when full");

  ptrs[0]->~Order();
  slab.deallocate(ptrs[0]);
  ptrs[0] = nullptr;

  Order *recovered = slab.allocate();
  check(recovered != nullptr, "pool should recover after freeing a slot");
  if (recovered == nullptr) {
    for (std::size_t i = 1; i < 8; ++i) {
      ptrs[i]->~Order();
      slab.deallocate(ptrs[i]);
    }
    return;
  }

  new (recovered) Order(make_order(99));
  check(recovered->id == 99, "recovered slot should be usable");
  recovered->~Order();
  slab.deallocate(recovered);

  for (std::size_t i = 1; i < 8; ++i) {
    ptrs[i]->~Order();
    slab.deallocate(ptrs[i]);
  }
  check(slab.available() == 8, "pool should be fully available after cleanup");
}

void test_all_allocated_pointers_are_distinct() {
  SmallSlab slab;
  std::set<Order *> seen;
  std::array<Order *, 8> ptrs{};

  for (std::size_t i = 0; i < 8; ++i) {
    ptrs[i] = slab.allocate();
    check(ptrs[i] != nullptr, "allocate should return a slot");
    if (ptrs[i] == nullptr) {
      return;
    }
    check(seen.insert(ptrs[i]).second,
          "allocate should never return a duplicate pointer");
    new (ptrs[i]) Order(make_order(static_cast<OrderId>(i)));
  }

  for (auto &p : ptrs) {
    p->~Order();
    slab.deallocate(p);
  }
}

void test_all_pointers_in_pool_range() {
  SmallSlab slab;
  std::array<Order *, 8> ptrs{};

  for (std::size_t i = 0; i < 8; ++i) {
    ptrs[i] = slab.allocate();
    check(ptrs[i] != nullptr, "allocate should return a slot");
    if (ptrs[i] == nullptr) {
      return;
    }
    check(slab.owns(ptrs[i]), "owned pointer should be inside the pool");
  }

  for (auto &p : ptrs) {
    p->~Order();
    slab.deallocate(p);
  }
}

void test_freed_slot_is_reused_immediately() {
  SmallSlab slab;

  Order *a = slab.allocate();
  Order *b = slab.allocate();
  check(a != nullptr && b != nullptr, "two allocations should succeed");
  if (a == nullptr || b == nullptr) {
    return;
  }

  new (a) Order(make_order(1));
  new (b) Order(make_order(2));

  b->~Order();
  slab.deallocate(b);

  Order *c = slab.allocate();
  check(c == b, "next allocation should reuse the last freed slot");
  if (c == nullptr) {
    a->~Order();
    slab.deallocate(a);
    return;
  }

  new (c) Order(make_order(3));

  a->~Order();
  slab.deallocate(a);
  c->~Order();
  slab.deallocate(c);
  check(slab.available() == 8, "pool should be fully available after cleanup");
}

void test_arbitrary_dealloc_order_preserves_all_slots() {
  SmallSlab slab;
  std::array<Order *, 8> ptrs{};

  for (std::size_t i = 0; i < 8; ++i) {
    ptrs[i] = slab.allocate();
    check(ptrs[i] != nullptr, "allocate should succeed while filling pool");
    if (ptrs[i] == nullptr) {
      return;
    }
    new (ptrs[i]) Order(make_order(static_cast<OrderId>(i)));
  }

  std::mt19937 rng(42);
  std::shuffle(ptrs.begin(), ptrs.end(), rng);

  for (auto &p : ptrs) {
    p->~Order();
    slab.deallocate(p);
  }
  check(slab.available() == 8, "shuffled frees should return all slots");

  std::array<Order *, 8> ptrs2{};
  for (std::size_t i = 0; i < 8; ++i) {
    ptrs2[i] = slab.allocate();
    check(ptrs2[i] != nullptr, "every slot should be re-allocatable");
    if (ptrs2[i] == nullptr) {
      return;
    }
    new (ptrs2[i]) Order(make_order(static_cast<OrderId>(i + 100)));
  }
  check(slab.is_full(), "pool should refill after shuffled frees");

  for (auto &p : ptrs2) {
    p->~Order();
    slab.deallocate(p);
  }
  check(slab.available() == 8, "pool should be fully available after cleanup");
}

void test_no_slot_lost_after_many_shuffled_cycles() {
  SmallSlab slab;
  std::mt19937 rng(1337);

  for (int cycle = 0; cycle < 10; ++cycle) {
    std::array<Order *, 8> ptrs{};
    for (auto &p : ptrs) {
      p = slab.allocate();
      check(p != nullptr, "allocate should succeed in every cycle");
      if (p == nullptr) {
        return;
      }
      new (p) Order(make_order(1));
    }

    std::shuffle(ptrs.begin(), ptrs.end(), rng);
    for (auto &p : ptrs) {
      p->~Order();
      slab.deallocate(p);
    }
    check(slab.available() == 8, "no slot should leak between cycles");
  }
}

void test_owns_accepts_pointers_from_pool() {
  SmallSlab slab;
  std::array<Order *, 8> ptrs{};

  for (std::size_t i = 0; i < 8; ++i) {
    ptrs[i] = slab.allocate();
    check(ptrs[i] != nullptr, "allocate should succeed");
    if (ptrs[i] == nullptr) {
      return;
    }
    check(slab.owns(ptrs[i]), "owns should accept pool pointers");
  }

  for (auto &p : ptrs) {
    p->~Order();
    slab.deallocate(p);
  }
}

void test_owns_rejects_stack_pointer() {
  SmallSlab slab;
  Order stack_order = make_order(99);
  check(!slab.owns(&stack_order), "owns should reject a stack pointer");
}

void test_owns_rejects_heap_pointer() {
  SmallSlab slab;
  Order *heap_order = new Order(make_order(99));
  check(!slab.owns(heap_order), "owns should reject a heap pointer");
  delete heap_order;
}

void test_owns_rejects_mid_slot_pointer() {
  SmallSlab slab;
  Order *a = slab.allocate();
  check(a != nullptr, "allocate should succeed");
  if (a == nullptr) {
    return;
  }
  new (a) Order(make_order(1));

  auto *mid = reinterpret_cast<Order *>(reinterpret_cast<char *>(a) + 1);
  check(!slab.owns(mid), "owns should reject a mid-slot pointer");

  a->~Order();
  slab.deallocate(a);
}

void test_owns_rejects_nullptr() {
  SmallSlab slab;
  check(!slab.owns(nullptr), "owns should reject nullptr");
}

void test_invalid_deallocation_does_not_change_state() {
  SmallSlab slab;
  Order foreign = make_order(88);

  check(!slab.deallocate(nullptr), "deallocate should reject nullptr");
  check(!slab.deallocate(&foreign), "deallocate should reject foreign pointers");
  check(slab.available() == 8,
        "invalid deallocate should not change availability");
  check(slab.allocated() == 0,
        "invalid deallocate should not change allocation count");

  Order *p = slab.allocate();
  check(p != nullptr, "allocate should succeed");
  if (p == nullptr) {
    return;
  }
  new (p) Order(make_order(89));

  auto *mid = reinterpret_cast<Order *>(reinterpret_cast<char *>(p) + 1);
  check(!slab.deallocate(mid), "deallocate should reject mid-slot pointers");
  check(slab.available() == 7,
        "rejected deallocate should leave the slot allocated");

  p->~Order();
  check(slab.deallocate(p), "deallocate should accept a valid slot");
}

void test_double_free_is_rejected_without_corrupting_free_list() {
  SmallSlab slab;
  Order *p = slab.allocate();
  check(p != nullptr, "allocate should succeed");
  if (p == nullptr) {
    return;
  }

  new (p) Order(make_order(90));
  p->~Order();
  check(slab.deallocate(p), "first deallocate should succeed");
  check(!slab.deallocate(p), "second deallocate should be rejected");
  check(slab.available() == 8, "double free should not change availability");
  check(slab.allocated() == 0, "double free should not change allocation count");

  std::set<Order *> seen;
  std::array<Order *, 8> slots{};
  for (auto &slot : slots) {
    slot = slab.allocate();
    check(slot != nullptr, "allocate should succeed after rejected double free");
    if (slot == nullptr) {
      return;
    }
    check(seen.insert(slot).second, "free list should not contain duplicates");
    new (slot) Order(make_order(91));
  }
  check(slab.allocate() == nullptr, "pool should still be exhaustible");

  for (auto *slot : slots) {
    slot->~Order();
    check(slab.deallocate(slot), "deallocate should succeed after refill");
  }
}

void test_poisoned_allocator_is_full_only_when_all_slots_are_allocated() {
  SlabAllocator<Order, 3> slab;
  Order *first = slab.allocate();
  Order *live = slab.allocate();
  check(first != nullptr && live != nullptr,
        "allocations should succeed before corrupting the free list");
  if (first == nullptr || live == nullptr) {
    return;
  }

  new (first) Order(make_order(92));
  new (live) Order(make_order(93));
  first->~Order();
  check(slab.deallocate(first), "first slot should be returned before corruption");

  void *invalid_next = reinterpret_cast<void *>(std::uintptr_t{1});
  std::memcpy(first, &invalid_next, sizeof(invalid_next));
  check(slab.allocate() == nullptr,
        "allocator should reject a free-list link outside the pool");
  check(slab.available() == 0,
        "poisoned allocator should expose no allocatable slots");
  check(slab.allocated() == 1,
        "poisoning should preserve the number of live caller allocations");
  check(!slab.is_full(),
        "poisoned allocator should not be full while a slot is unallocated");
  check(slab.allocate() == nullptr,
        "poisoned allocator should reject subsequent allocations");
  live->~Order();
}

void test_supports_over_aligned_types() {
  SlabAllocator<OverAligned, 4> slab;
  std::array<OverAligned *, 4> slots{};

  for (auto &slot : slots) {
    slot = slab.allocate();
    check(slot != nullptr, "allocate should succeed for over-aligned type");
    if (slot == nullptr) {
      return;
    }
    check(reinterpret_cast<std::uintptr_t>(slot) % alignof(OverAligned) == 0,
          "slots should satisfy the type alignment");
    new (slot) OverAligned{42};
    check(slot->value == 42, "over-aligned slot should be writable");
  }

  for (auto *slot : slots) {
    slot->~OverAligned();
    check(slab.deallocate(slot), "deallocate should succeed");
  }
}

void test_caller_controls_construction_and_destruction() {
  LifetimeTracked::constructions = 0;
  LifetimeTracked::destructions = 0;

  SlabAllocator<LifetimeTracked, 2> slab;
  LifetimeTracked *p = slab.allocate();
  check(p != nullptr, "allocate should return raw storage");
  if (p == nullptr) {
    return;
  }

  check(LifetimeTracked::constructions == 0,
        "allocate should not construct the object");
  new (p) LifetimeTracked(123);
  check(LifetimeTracked::constructions == 1,
        "placement new should construct the object");
  check(p->value == 123, "constructed value should be readable");

  p->~LifetimeTracked();
  check(LifetimeTracked::destructions == 1,
        "caller should control destruction");
  check(slab.deallocate(p), "deallocate should succeed");
}

void test_capacity_one() {
  SlabAllocator<Order, 1> slab;
  check(slab.capacity() == 1, "capacity should be 1");
  check(slab.available() == 1, "single slot should be available");

  Order *p = slab.allocate();
  check(p != nullptr, "allocate should succeed");
  if (p == nullptr) {
    return;
  }
  new (p) Order(make_order(7));
  check(slab.available() == 0, "single slot should be consumed");
  check(slab.is_full(), "capacity-one pool should be full");
  check(slab.allocate() == nullptr, "second allocate should fail");

  p->~Order();
  slab.deallocate(p);
  check(slab.available() == 1, "deallocate should restore the single slot");

  Order *p2 = slab.allocate();
  check(p2 != nullptr, "allocate should succeed after deallocate");
  if (p2 == nullptr) {
    return;
  }
  new (p2) Order(make_order(8));
  p2->~Order();
  slab.deallocate(p2);
}

void test_large_capacity_no_leak() {
  constexpr std::size_t N = 1024;
  SlabAllocator<Order, N> slab;
  check(slab.available() == N, "large pool should start fully available");

  std::array<Order *, N> ptrs{};
  for (std::size_t i = 0; i < N; ++i) {
    ptrs[i] = slab.allocate();
    check(ptrs[i] != nullptr, "allocate should succeed while filling pool");
    if (ptrs[i] == nullptr) {
      return;
    }
    new (ptrs[i]) Order(make_order(static_cast<OrderId>(i)));
  }
  check(slab.is_full(), "large pool should become full");
  check(slab.allocate() == nullptr, "full large pool should reject allocate");

  for (auto &p : ptrs) {
    p->~Order();
    slab.deallocate(p);
  }
  check(slab.available() == N, "large pool should not leak slots");
}

void test_data_survives_alloc_dealloc() {
  SmallSlab slab;

  Order *a = slab.allocate();
  Order *b = slab.allocate();
  Order *c = slab.allocate();
  check(a != nullptr && b != nullptr && c != nullptr,
        "three allocations should succeed");
  if (a == nullptr || b == nullptr || c == nullptr) {
    return;
  }

  new (a) Order(make_order(10, Side::Buy, 500, 20));
  new (b) Order(make_order(11, Side::Sell, 510, 30));
  new (c) Order(make_order(12, Side::Buy, 495, 15));

  b->~Order();
  slab.deallocate(b);
  Order *d = slab.allocate();
  check(d != nullptr, "allocate should reuse the freed slot");
  if (d == nullptr) {
    a->~Order();
    slab.deallocate(a);
    c->~Order();
    slab.deallocate(c);
    return;
  }
  new (d) Order(make_order(99, Side::Sell, 600, 5));

  check(a->id == 10, "first slot id should be untouched");
  check(a->price == 500, "first slot price should be untouched");
  check(a->quantity == 20, "first slot quantity should be untouched");

  check(c->id == 12, "third slot id should be untouched");
  check(c->price == 495, "third slot price should be untouched");
  check(c->quantity == 15, "third slot quantity should be untouched");

  check(d->id == 99, "reused slot should contain new data");

  a->~Order();
  slab.deallocate(a);
  c->~Order();
  slab.deallocate(c);
  d->~Order();
  slab.deallocate(d);
}

} // namespace

int main() {
  test_allocate_returns_non_null();
  test_deallocate_restores_availability();
  test_multiple_alloc_dealloc_cycles();
  test_counters_consistent_throughout_lifecycle();
  test_returns_nullptr_when_full();
  test_recovery_after_exhaustion();
  test_all_allocated_pointers_are_distinct();
  test_all_pointers_in_pool_range();
  test_freed_slot_is_reused_immediately();
  test_arbitrary_dealloc_order_preserves_all_slots();
  test_no_slot_lost_after_many_shuffled_cycles();
  test_owns_accepts_pointers_from_pool();
  test_owns_rejects_stack_pointer();
  test_owns_rejects_heap_pointer();
  test_owns_rejects_mid_slot_pointer();
  test_owns_rejects_nullptr();
  test_invalid_deallocation_does_not_change_state();
  test_double_free_is_rejected_without_corrupting_free_list();
  test_poisoned_allocator_is_full_only_when_all_slots_are_allocated();
  test_supports_over_aligned_types();
  test_caller_controls_construction_and_destruction();
  test_capacity_one();
  test_large_capacity_no_leak();
  test_data_survives_alloc_dealloc();

  if (failures != 0) {
    std::cerr << failures << " test check(s) failed.\n";
    return 1;
  }

  std::cout << "All slab allocator tests passed.\n";
  return 0;
}
