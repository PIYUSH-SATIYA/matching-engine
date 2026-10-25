// ---------------------------------------------------------------------------
// tests/test_slab_allocator.cpp
//
// Unit tests for SlabAllocator<T, Capacity>
// Iteration 4 — Memory Management (Member C: Sanskar Sharma)
//
// Documented requirements verified here
// ──────────────────────────────────────
// 1. Basic alloc / dealloc — allocate, write, read back, deallocate.
// 2. Pool exhaustion        — allocate returns nullptr when full.
// 3. Unique slot addresses  — no two concurrent allocations alias.
// 4. LIFO slot reuse        — freed slot is immediately reused on next alloc.
// 5. Out-of-order free      — free in arbitrary order, refill to Capacity.
// 6. owns() correctness     — accepts pool ptrs, rejects alien ptrs.
// 7. Edge cases             — Capacity == 1, mid-slot alien ptr, boundary ptrs.
// 8. Counters               — available(), allocated(), is_full() are consistent.
//
// Build (host, no cmake):
//   g++ -std=c++20 -Wall -Wextra -fsanitize=address,undefined
//       -I include tests/test_slab_allocator.cpp
//       -lgtest -lgtest_main -pthread -o test_slab_allocator
// Run:
//   ./test_slab_allocator
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "slab_allocator.hpp"
#include "order.hpp"

#include <algorithm>    // std::shuffle
#include <array>
#include <cstdint>
#include <random>
#include <set>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Make a fully-initialised Order so sanitisers can verify field reads.
static Order make_order(OrderId id, Side side = Side::Buy,
                        Price price = 100, Quantity qty = 10) {
    return Order{id, side, price, qty, {}};
}

// Shorthand allocator type used by most tests.
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

// ---------------------------------------------------------------------------
// Test suite 1 — Basic allocation and deallocation
// ---------------------------------------------------------------------------

TEST(SlabAllocatorBasic, AllocateReturnsNonNull) {
    SmallSlab slab;
    Order* p = slab.allocate();
    ASSERT_NE(p, nullptr);
    // Construct and read back to let ASan verify the memory is writable.
    new (p) Order(make_order(1));
    EXPECT_EQ(p->id, 1u);
    p->~Order();
    slab.deallocate(p);
}

TEST(SlabAllocatorBasic, DeallocateRestoresAvailability) {
    SmallSlab slab;
    EXPECT_EQ(slab.available(), 8u);
    Order* p = slab.allocate();
    EXPECT_EQ(slab.available(), 7u);
    EXPECT_EQ(slab.allocated(), 1u);
    new (p) Order(make_order(2));
    p->~Order();
    slab.deallocate(p);
    EXPECT_EQ(slab.available(), 8u);
    EXPECT_EQ(slab.allocated(), 0u);
}

TEST(SlabAllocatorBasic, MultipleAllocDeallocCycles) {
    SmallSlab slab;
    // Run 5 allocate → use → deallocate cycles; pool must stay consistent.
    for (int cycle = 0; cycle < 5; ++cycle) {
        Order* p = slab.allocate();
        ASSERT_NE(p, nullptr) << "cycle " << cycle;
        new (p) Order(make_order(static_cast<OrderId>(cycle)));
        EXPECT_EQ(p->id, static_cast<OrderId>(cycle));
        p->~Order();
        slab.deallocate(p);
        EXPECT_EQ(slab.available(), 8u);
    }
}

TEST(SlabAllocatorBasic, CountersConsistentThroughoutLifecycle) {
    SmallSlab slab;
    EXPECT_EQ(slab.capacity(),  8u);
    EXPECT_EQ(slab.available(), 8u);
    EXPECT_EQ(slab.allocated(), 0u);
    EXPECT_FALSE(slab.is_full());

    std::array<Order*, 8> ptrs{};
    for (std::size_t i = 0; i < 8; ++i) {
        ptrs[i] = slab.allocate();
        ASSERT_NE(ptrs[i], nullptr);
        new (ptrs[i]) Order(make_order(static_cast<OrderId>(i + 100)));
        EXPECT_EQ(slab.available(), 8 - i - 1);
        EXPECT_EQ(slab.allocated(), i + 1);
    }
    EXPECT_TRUE(slab.is_full());

    for (std::size_t i = 0; i < 8; ++i) {
        ptrs[i]->~Order();
        slab.deallocate(ptrs[i]);
        EXPECT_EQ(slab.available(), i + 1);
        EXPECT_EQ(slab.allocated(), 8 - i - 1);
    }
    EXPECT_FALSE(slab.is_full());
}

// ---------------------------------------------------------------------------
// Test suite 2 — Pool exhaustion (documented requirement)
// ---------------------------------------------------------------------------

TEST(SlabAllocatorExhaustion, ReturnsNullptrWhenFull) {
    SmallSlab slab;
    std::array<Order*, 8> ptrs{};
    for (auto& p : ptrs) {
        p = slab.allocate();
        ASSERT_NE(p, nullptr);
        new (p) Order(make_order(1));
    }
    EXPECT_TRUE(slab.is_full());

    // The (Capacity + 1)-th allocation must return nullptr, no crash.
    Order* overflow = slab.allocate();
    EXPECT_EQ(overflow, nullptr);

    // Cleanup.
    for (auto& p : ptrs) { p->~Order(); slab.deallocate(p); }
}

TEST(SlabAllocatorExhaustion, RecoveryAfterExhaustion) {
    SmallSlab slab;
    std::array<Order*, 8> ptrs{};
    for (auto& p : ptrs) { p = slab.allocate(); new (p) Order(make_order(1)); }

    EXPECT_EQ(slab.allocate(), nullptr);

    // Free one slot — pool should recover.
    ptrs[0]->~Order();
    slab.deallocate(ptrs[0]);
    ptrs[0] = nullptr;

    Order* recovered = slab.allocate();
    ASSERT_NE(recovered, nullptr);
    new (recovered) Order(make_order(99));
    EXPECT_EQ(recovered->id, 99u);

    // Cleanup remaining.
    recovered->~Order(); slab.deallocate(recovered);
    for (std::size_t i = 1; i < 8; ++i) {
        ptrs[i]->~Order(); slab.deallocate(ptrs[i]);
    }
    EXPECT_EQ(slab.available(), 8u);
}

// ---------------------------------------------------------------------------
// Test suite 3 — Unique slot addresses
// ---------------------------------------------------------------------------

TEST(SlabAllocatorUniqueness, AllAllocatedPointersAreDistinct) {
    SmallSlab slab;
    std::set<Order*> seen;
    std::array<Order*, 8> ptrs{};

    for (std::size_t i = 0; i < 8; ++i) {
        ptrs[i] = slab.allocate();
        ASSERT_NE(ptrs[i], nullptr);
        // Each pointer must be unique — no aliasing.
        EXPECT_TRUE(seen.insert(ptrs[i]).second)
            << "Duplicate pointer returned at index " << i;
        new (ptrs[i]) Order(make_order(static_cast<OrderId>(i)));
    }

    for (auto& p : ptrs) { p->~Order(); slab.deallocate(p); }
}

TEST(SlabAllocatorUniqueness, AllPointersInPoolRange) {
    SmallSlab slab;
    std::array<Order*, 8> ptrs{};
    for (std::size_t i = 0; i < 8; ++i) {
        ptrs[i] = slab.allocate();
        ASSERT_NE(ptrs[i], nullptr);
        EXPECT_TRUE(slab.owns(ptrs[i]))
            << "Returned pointer not owned by pool at index " << i;
    }
    for (auto& p : ptrs) { p->~Order(); slab.deallocate(p); }
}

// ---------------------------------------------------------------------------
// Test suite 4 — LIFO slot reuse (free list is LIFO by design)
// ---------------------------------------------------------------------------

TEST(SlabAllocatorReuse, FreedSlotIsReusedImmediately) {
    SmallSlab slab;

    Order* a = slab.allocate();
    Order* b = slab.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    new (a) Order(make_order(1));
    new (b) Order(make_order(2));

    // Deallocate b — it goes to the head of the free list.
    b->~Order();
    slab.deallocate(b);

    // Next allocation must return the slot that was b (LIFO).
    Order* c = slab.allocate();
    EXPECT_EQ(c, b) << "Expected LIFO reuse of last-freed slot";
    new (c) Order(make_order(3));

    // Cleanup.
    a->~Order(); slab.deallocate(a);
    c->~Order(); slab.deallocate(c);
    EXPECT_EQ(slab.available(), 8u);
}

// ---------------------------------------------------------------------------
// Test suite 5 — Out-of-order freeing (documented requirement)
// ---------------------------------------------------------------------------

TEST(SlabAllocatorOutOfOrderFree, ArbitraryDeallocOrderPreservesAllSlots) {
    SmallSlab slab;
    std::array<Order*, 8> ptrs{};

    for (std::size_t i = 0; i < 8; ++i) {
        ptrs[i] = slab.allocate();
        ASSERT_NE(ptrs[i], nullptr);
        new (ptrs[i]) Order(make_order(static_cast<OrderId>(i)));
    }

    // Shuffle the pointers so they are freed in a random order.
    std::mt19937 rng(42);  // fixed seed for reproducibility
    std::shuffle(ptrs.begin(), ptrs.end(), rng);

    for (auto& p : ptrs) {
        p->~Order();
        slab.deallocate(p);
    }
    EXPECT_EQ(slab.available(), 8u);

    // After shuffled free, every slot must be re-allocatable.
    std::array<Order*, 8> ptrs2{};
    for (std::size_t i = 0; i < 8; ++i) {
        ptrs2[i] = slab.allocate();
        ASSERT_NE(ptrs2[i], nullptr) << "slot " << i << " not re-allocatable";
        new (ptrs2[i]) Order(make_order(static_cast<OrderId>(i + 100)));
    }
    EXPECT_TRUE(slab.is_full());

    for (auto& p : ptrs2) { p->~Order(); slab.deallocate(p); }
    EXPECT_EQ(slab.available(), 8u);
}

TEST(SlabAllocatorOutOfOrderFree, NoSlotLostAfterManyShuffledCycles) {
    // Run 10 full allocate-shuffle-free cycles; pool must always return to
    // Capacity available slots (no "lost" slots).
    SmallSlab slab;
    std::mt19937 rng(1337);

    for (int cycle = 0; cycle < 10; ++cycle) {
        std::array<Order*, 8> ptrs{};
        for (auto& p : ptrs) {
            p = slab.allocate();
            ASSERT_NE(p, nullptr) << "cycle " << cycle;
            new (p) Order(make_order(1));
        }
        std::shuffle(ptrs.begin(), ptrs.end(), rng);
        for (auto& p : ptrs) { p->~Order(); slab.deallocate(p); }
        EXPECT_EQ(slab.available(), 8u) << "slot leak in cycle " << cycle;
    }
}

// ---------------------------------------------------------------------------
// Test suite 6 — owns() correctness
// ---------------------------------------------------------------------------

TEST(SlabAllocatorOwns, AcceptsPointersFromPool) {
    SmallSlab slab;
    std::array<Order*, 8> ptrs{};
    for (std::size_t i = 0; i < 8; ++i) {
        ptrs[i] = slab.allocate();
        EXPECT_TRUE(slab.owns(ptrs[i])) << "index " << i;
    }
    for (auto& p : ptrs) { p->~Order(); slab.deallocate(p); }
}

TEST(SlabAllocatorOwns, RejectsStackPointer) {
    SmallSlab slab;
    Order stack_order = make_order(99);
    EXPECT_FALSE(slab.owns(&stack_order));
}

TEST(SlabAllocatorOwns, RejectsHeapPointer) {
    SmallSlab slab;
    Order* heap_order = new Order(make_order(99));
    EXPECT_FALSE(slab.owns(heap_order));
    delete heap_order;
}

TEST(SlabAllocatorOwns, RejectsMidSlotPointer) {
    // A pointer that is inside pool boundaries but NOT on a slot boundary
    // should be rejected by the alignment check in owns().
    SmallSlab slab;
    Order* a = slab.allocate();
    ASSERT_NE(a, nullptr);
    new (a) Order(make_order(1));

    // Create a byte-offset pointer into the middle of slot 0.
    // We intentionally cast through char* to do pointer arithmetic.
    // owns() should return false for mid-slot addresses.
    auto* mid = reinterpret_cast<Order*>(
        reinterpret_cast<char*>(a) + 1
    );
    EXPECT_FALSE(slab.owns(mid));

    a->~Order();
    slab.deallocate(a);
}

TEST(SlabAllocatorOwns, RejectsNullptr) {
    SmallSlab slab;
    // owns() must not crash on nullptr; it should return false.
    EXPECT_FALSE(slab.owns(nullptr));
}

TEST(SlabAllocatorOwns, InvalidDeallocationDoesNotChangeState) {
    SmallSlab slab;
    Order foreign = make_order(88);
    EXPECT_FALSE(slab.deallocate(nullptr));
    EXPECT_FALSE(slab.deallocate(&foreign));
    EXPECT_EQ(slab.available(), 8u);
    EXPECT_EQ(slab.allocated(), 0u);
    Order* p = slab.allocate();
    ASSERT_NE(p, nullptr);
    new (p) Order(make_order(89));
    EXPECT_FALSE(slab.deallocate(reinterpret_cast<Order*>(
        reinterpret_cast<char*>(p) + 1)));
    EXPECT_EQ(slab.available(), 7u);
    p->~Order();
    EXPECT_TRUE(slab.deallocate(p));
}

TEST(SlabAllocatorOwns, DoubleFreeIsRejectedWithoutCorruptingFreeList) {
    SmallSlab slab;
    Order* p = slab.allocate();
    ASSERT_NE(p, nullptr);
    new (p) Order(make_order(90));
    p->~Order();
    ASSERT_TRUE(slab.deallocate(p));
    EXPECT_FALSE(slab.deallocate(p));
    EXPECT_EQ(slab.available(), 8u);
    EXPECT_EQ(slab.allocated(), 0u);

    std::set<Order*> seen;
    std::array<Order*, 8> slots{};
    for (auto& slot : slots) {
        slot = slab.allocate();
        ASSERT_NE(slot, nullptr);
        EXPECT_TRUE(seen.insert(slot).second);
        new (slot) Order(make_order(91));
    }
    EXPECT_EQ(slab.allocate(), nullptr);
    for (auto* slot : slots) {
        slot->~Order();
        EXPECT_TRUE(slab.deallocate(slot));
    }
}

TEST(SlabAllocatorAlignment, SupportsOverAlignedTypes) {
    SlabAllocator<OverAligned, 4> slab;
    std::array<OverAligned*, 4> slots{};
    for (auto& slot : slots) {
        slot = slab.allocate();
        ASSERT_NE(slot, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(slot) % alignof(OverAligned), 0u);
        new (slot) OverAligned{42};
        EXPECT_EQ(slot->value, 42u);
    }
    for (auto* slot : slots) {
        slot->~OverAligned();
        EXPECT_TRUE(slab.deallocate(slot));
    }
}

TEST(SlabAllocatorLifetime, CallerControlsConstructionAndDestruction) {
    LifetimeTracked::constructions = 0;
    LifetimeTracked::destructions = 0;
    SlabAllocator<LifetimeTracked, 2> slab;
    LifetimeTracked* p = slab.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(LifetimeTracked::constructions, 0);
    new (p) LifetimeTracked(123);
    EXPECT_EQ(LifetimeTracked::constructions, 1);
    EXPECT_EQ(p->value, 123u);
    p->~LifetimeTracked();
    EXPECT_EQ(LifetimeTracked::destructions, 1);
    EXPECT_TRUE(slab.deallocate(p));
}

// ---------------------------------------------------------------------------
// Test suite 7 — Edge cases
// ---------------------------------------------------------------------------

TEST(SlabAllocatorEdge, CapacityOne) {
    // Pool of size 1: allocate succeeds, second returns nullptr, dealloc
    // restores, allocate succeeds again.
    SlabAllocator<Order, 1> slab;
    EXPECT_EQ(slab.capacity(),  1u);
    EXPECT_EQ(slab.available(), 1u);

    Order* p = slab.allocate();
    ASSERT_NE(p, nullptr);
    new (p) Order(make_order(7));
    EXPECT_EQ(slab.available(), 0u);
    EXPECT_TRUE(slab.is_full());

    EXPECT_EQ(slab.allocate(), nullptr);

    p->~Order();
    slab.deallocate(p);
    EXPECT_EQ(slab.available(), 1u);

    Order* p2 = slab.allocate();
    ASSERT_NE(p2, nullptr);
    new (p2) Order(make_order(8));
    p2->~Order();
    slab.deallocate(p2);
}

TEST(SlabAllocatorEdge, LargeCapacityNoLeak) {
    // Capacity 1024: full fill then full free — no counter leak.
    constexpr std::size_t N = 1024;
    SlabAllocator<Order, N> slab;
    EXPECT_EQ(slab.available(), N);

    std::array<Order*, N> ptrs{};
    for (std::size_t i = 0; i < N; ++i) {
        ptrs[i] = slab.allocate();
        ASSERT_NE(ptrs[i], nullptr) << "slot " << i;
        new (ptrs[i]) Order(make_order(static_cast<OrderId>(i)));
    }
    EXPECT_TRUE(slab.is_full());
    EXPECT_EQ(slab.allocate(), nullptr);

    for (auto& p : ptrs) { p->~Order(); slab.deallocate(p); }
    EXPECT_EQ(slab.available(), N);
}

TEST(SlabAllocatorEdge, DataSurvivesAllocDealloc) {
    // Verify data written into an allocated slot is not corrupted by
    // subsequent alloc/dealloc of other slots (no clobbering via free list).
    SmallSlab slab;

    Order* a = slab.allocate();
    Order* b = slab.allocate();
    Order* c = slab.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    new (a) Order(make_order(10, Side::Buy,  500, 20));
    new (b) Order(make_order(11, Side::Sell, 510, 30));
    new (c) Order(make_order(12, Side::Buy,  495, 15));

    // Free and re-allocate b; a and c must be undisturbed.
    b->~Order();
    slab.deallocate(b);
    Order* d = slab.allocate(); // should reuse b's slot
    new (d) Order(make_order(99, Side::Sell, 600, 5));

    EXPECT_EQ(a->id,       10u);
    EXPECT_EQ(a->price,    500);
    EXPECT_EQ(a->quantity, 20u);

    EXPECT_EQ(c->id,       12u);
    EXPECT_EQ(c->price,    495);
    EXPECT_EQ(c->quantity, 15u);

    EXPECT_EQ(d->id,       99u);

    a->~Order(); slab.deallocate(a);
    c->~Order(); slab.deallocate(c);
    d->~Order(); slab.deallocate(d);
}
