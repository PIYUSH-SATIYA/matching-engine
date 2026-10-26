#pragma once

// ---------------------------------------------------------------------------
// slab_allocator.hpp
//
// Iteration 4 — Memory Management (Owner: Sanskar Sharma, Member C)
//
// Purpose
// -------
// A fixed-size, single-threaded slab / pool allocator for objects of type T.
// The allocator pre-allocates one contiguous block of (Capacity * sizeof(T))
// bytes at construction time and hands out fixed-size slots using an in-place
// free list.  Every allocate() and deallocate() is O(1) and makes no system
// calls — removing the non-deterministic latency spikes that glibc malloc
// introduces into the matching engine's hot path.
//
// Design choices (mapped to project documentation)
// -------------------------------------------------
// * In-place free list (documented requirement):
//     When a slot is free, the first sizeof(void*) bytes of that slot store a
//     pointer to the *next* free slot.  No separate bookkeeping array is
//     needed.  This works because sizeof(T) >= sizeof(void*) is enforced by a
//     compile-time assertion.
//
// * Aligned contiguous storage (documented requirement):
//     pool_ is declared as alignas(alignof(T)) std::byte[SlotSize * Capacity].
//     Every slot therefore starts at an address that satisfies T's alignment
//     requirement.  alignof(Order) == 8 on the target machine.
//
// * No locks (documented requirement, Phase 1 Component Contract §10):
//     The Matching Engine is single-threaded; no synchronization is needed
//     inside the allocator.
//
// * nullptr on exhaustion (documented requirement):
//     When all Capacity slots are in use, allocate() returns nullptr.  The
//     caller is responsible for handling this condition.
//
// * Non-copyable, non-movable:
//     The allocator owns its pool_.  Copying or moving would invalidate all
//     outstanding pointers held by callers.
//
// Additional safety helpers (Antigravity recommendations, not required by docs)
// ------------------------------------------------------------------------------
// * owns(ptr)     — checks whether a pointer denotes a slot boundary in pool
// * is_full()     — true when no free slot remains
// * available()   — number of free slots remaining
// * allocated()   — number of slots currently in use
// * capacity()    — compile-time constant: total number of slots
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// SlabAllocator<T, Capacity>
//
//   T        — the object type to allocate (e.g. Order).
//               Must satisfy sizeof(T) >= sizeof(void*) — enforced by
//               static_assert below.
//
//   Capacity — maximum number of live T objects the pool can hold at once.
//              Choose a value larger than the expected peak resting-order
//              depth in the order book.
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
class SlabAllocator {

  // ── Compile-time safety checks ───────────────────────────────────────────

  static_assert(Capacity > 0,
    "SlabAllocator: Capacity must be at least 1.");

  static_assert(sizeof(T) >= sizeof(void*),
    "SlabAllocator: sizeof(T) must be >= sizeof(void*) so that the "
    "in-place free list can store a next-pointer inside each free slot. "
    "Either use a larger T or redesign the free list.");

  // ── Internal constant ────────────────────────────────────────────────────

  // SlotSize — allocation unit; exactly sizeof(T) bytes per slot.
  // pool_ is sized SlotSize * Capacity bytes total.
  static constexpr std::size_t SlotSize = sizeof(T);

public:
  // ── Construction / Destruction ───────────────────────────────────────────

  // Constructor: O(Capacity) one-time work, called at engine startup —
  // outside the hot path.
  //
  // Builds the free list by threading every slot in index order:
  //   Slot 0  →  Slot 1  →  …  →  Slot (Capacity-1)  →  nullptr
  //
  // std::memcpy is used (instead of pointer-cast assignments) to write
  // pointer values into std::byte storage without violating strict-aliasing
  // rules.
  SlabAllocator() noexcept {
    for (std::size_t i = 0; i < Capacity - 1; ++i) {
      void* current = slot_ptr(i);
      void* next    = slot_ptr(i + 1);
      // Write the address of the next slot into current slot's raw bytes.
      std::memcpy(current, &next, sizeof(void*));
    }
    // Last slot terminates the list.
    void* null_ptr = nullptr;
    std::memcpy(slot_ptr(Capacity - 1), &null_ptr, sizeof(void*));

    free_head_  = slot_ptr(0);
    free_count_ = Capacity;
  }

  // Destructor: in debug builds, poison the pool with 0xCD so that any
  // access through a dangling T* produces a visibly bad value rather than
  // silent stale data.
  ~SlabAllocator() noexcept {
#ifndef NDEBUG
    std::memset(pool_, 0xCD, sizeof(pool_));
    free_head_  = nullptr;
    free_count_ = 0;
#endif
  }

  // Non-copyable and non-movable.
  // The pool_ address is embedded in every outstanding T* issued to callers;
  // moving the allocator would silently invalidate all of them.
  SlabAllocator(const SlabAllocator&)            = delete;
  SlabAllocator& operator=(const SlabAllocator&) = delete;
  SlabAllocator(SlabAllocator&&)                 = delete;
  SlabAllocator& operator=(SlabAllocator&&)      = delete;

  // ── Core interface (documented requirements) ─────────────────────────────

  // allocate()
  //
  // Returns a pointer to an uninitialised, properly aligned block of memory
  // sized for exactly one T, or nullptr when the pool is exhausted.
  //
  // Usage — caller constructs the object via placement-new:
  //   T* p = slab.allocate();
  //   if (p) { new (p) T{ .id = 1, .side = Side::Buy, ... }; }
  //
  // Complexity: O(1).  No system calls, no locks, no heap interaction.
  [[nodiscard]] T* allocate() noexcept {
    if (free_head_ == nullptr) {
      // Pool exhausted — documented requirement: return nullptr, no throw.
      return nullptr;
    }

    // Pop the head slot off the free list.
    void* slot = free_head_;

    // Read the next-pointer that was stored inside this slot when it was free.
    void* next = nullptr;
    std::memcpy(&next, slot, sizeof(void*));
    free_head_ = next;

    --free_count_;

    const std::size_t index = slot_index(slot);
    allocated_[index] = true;
    // The returned pointer denotes storage; callers begin T's lifetime with
    // placement new before accessing it.
    return reinterpret_cast<T*>(slot);
  }

  // deallocate(ptr)
  //
  // Returns the memory occupied by *ptr back to the pool, true on success.
  //
  // IMPORTANT: the caller must explicitly destroy the object before calling
  // deallocate, because the allocator does not call destructors:
  //   ptr->~T();
  //   slab.deallocate(ptr);
  //
  // Invalid, foreign, or already-freed pointers are rejected with false and
  // leave allocator state unchanged. The caller must destroy a live T first.
  //
  // Complexity: O(1).  No system calls, no locks, no heap interaction.
  bool deallocate(T* ptr) noexcept {
    if (!owns(ptr)) {
      return false;
    }
    const std::size_t index = slot_index(ptr);
    if (!allocated_[index]) {
      return false;
    }

    // Push freed slot onto the head of the free list.
    // Write the current free_head_ value into the slot's raw bytes, then
    // point free_head_ at the now-free slot.
    void* raw = static_cast<void*>(ptr);
    std::memcpy(raw, &free_head_, sizeof(void*));
    free_head_ = raw;

    allocated_[index] = false;
    ++free_count_;
    return true;
  }

  // ── Diagnostic / safety helpers (Antigravity recommendations) ────────────

  // owns(ptr)
  //
  // Returns true iff ptr addresses the start of a valid slot inside pool_.
  //
  // Two conditions must both hold:
  //   (a) ptr lies within [pool_start, pool_end).
  //   (b) ptr's byte offset from pool_start is a multiple of SlotSize —
  //       i.e. ptr lands exactly on a slot boundary, not mid-slot.
  //
  // Useful for validation and tests; does not indicate whether the slot is
  // currently allocated.
  [[nodiscard]] bool owns(const T* ptr) const noexcept {
    if (ptr == nullptr) {
      return false;
    }
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(pool_);
    if (address < start) {
      return false;
    }
    const std::uintptr_t offset = address - start;
    return offset < sizeof(pool_) && (offset % SlotSize) == 0;
  }

  // is_full() — true when every slot is currently held by a caller.
  [[nodiscard]] bool is_full() const noexcept {
    return free_count_ == 0;
  }

  // available() — number of slots that can still be allocated.
  [[nodiscard]] std::size_t available() const noexcept {
    return free_count_;
  }

  // allocated() — number of slots currently held by callers.
  [[nodiscard]] std::size_t allocated() const noexcept {
    return Capacity - free_count_;
  }

  // capacity() — compile-time total slot count.
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

private:
  // ── Private helper ────────────────────────────────────────────────────────

  // slot_ptr(i) — raw void* pointing to the first byte of slot i.
  void* slot_ptr(std::size_t i) noexcept {
    return static_cast<void*>(pool_ + SlotSize * i);
  }

  std::size_t slot_index(const void* ptr) const noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    const auto start = reinterpret_cast<std::uintptr_t>(pool_);
    return static_cast<std::size_t>((address - start) / SlotSize);
  }

  // ── Storage ───────────────────────────────────────────────────────────────

  // pool_ — the pre-allocated contiguous byte buffer.
  //
  // alignas(alignof(T)) ensures pool_[0] meets T's alignment requirement.
  // Subsequent slots begin at offsets 1*SlotSize, 2*SlotSize, … which are
  // all multiples of alignof(T) because sizeof(T) is always a multiple of
  // alignof(T) by the C++ standard (§6.7.1 [basic.types.general]).
  alignas(alignof(T)) std::byte pool_[SlotSize * Capacity];

  // free_head_ — pointer to the first free slot, or nullptr when pool is full.
  void* free_head_;

  // Per-slot state makes invalid and duplicate frees detectable in O(1).
  bool allocated_[Capacity]{};

  // free_count_ — maintained in O(1) alongside free_head_.
  // Not required by the docs, but invaluable for assertions and benchmarking.
  std::size_t free_count_;
};
