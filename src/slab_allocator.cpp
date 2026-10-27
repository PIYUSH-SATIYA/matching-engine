// ---------------------------------------------------------------------------
// src/slab_allocator.cpp
//
// Iteration 4 — Memory Management (Owner: Sanskar Sharma, Member C)
//
// Why does this file exist?
// ─────────────────────────
// SlabAllocator<T, Capacity> is a class template: all method bodies live in
// slab_allocator.hpp and are compiled wherever that header is included.
//
// This translation unit fulfils two roles:
//
//   1. Explicit template instantiation for the canonical project type.
//      The directive below forces the compiler to emit fully-compiled object
//      code for SlabAllocator<Order, ORDER_POOL_CAPACITY> exactly once —
//      here. This is useful as a canonical instantiation anchor. Because the
//      header has no matching extern-template declaration, TUs that use this
//      specialization may still instantiate it themselves.
//
//   2. Build-system anchor.
//      The project plan (docs/init/team-lead-project-plan.md, §0) lists
//      src/slab_allocator.cpp in the matching_engine executable's source
//      list.  Without at least one translation unit, CMake cannot produce
//      an object file for the module.  This file satisfies that requirement
//      without duplicating any logic from the header.
//
// Usage in production code (matching engine / order book):
//
//   #include "slab_allocator.hpp"
//   #include "order.hpp"
//
//   // A single, shared pool sized for ORDER_POOL_CAPACITY resting orders.
//   // Defined here to give it a well-known home; the OrderBook will hold a
//   // reference or pointer to it.
//   SlabAllocator<Order, ORDER_POOL_CAPACITY> g_order_pool;
//
// ---------------------------------------------------------------------------

#include "slab_allocator.hpp"
#include "order.hpp"

// ---------------------------------------------------------------------------
// Canonical pool capacity
//
// The order book at peak load must be able to hold all resting orders on both
// sides simultaneously.  A realistic workload for a single-symbol order book
// in an exchange simulation is O(10^4) resting orders.  We choose 65 536
// (2^16) as the smallest power-of-two that comfortably exceeds that and
// keeps the pool footprint manageable:
//
//   Pool size = 65 536 * sizeof(Order). With the current Order layout this is
//   2 621 440 bytes (40 bytes per Order), plus the allocator's state array.
//
// If the matching engine is later extended to support multiple symbols, each
// symbol gets its own SlabAllocator<Order, ORDER_POOL_CAPACITY> instance.
// ---------------------------------------------------------------------------
// This capacity is a project choice: the docs specify a configurable N sized
// for expected peak depth, but do not mandate a particular numeric value.
static constexpr std::size_t ORDER_POOL_CAPACITY = 65'536;

// Explicit template instantiation.
//
// This tells the compiler: "generate full object code for every method of
// SlabAllocator<Order, ORDER_POOL_CAPACITY> right here."  All other TUs that
// include slab_allocator.hpp can use this specialization; without an
// extern-template declaration, implicit instantiation remains permitted in
// those translation units.
//
// C++ standard reference: [temp.explicit] (ISO/IEC 14882:2020 §13.9.2)
template class SlabAllocator<Order, ORDER_POOL_CAPACITY>;
