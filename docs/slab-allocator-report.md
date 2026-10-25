# Slab Allocator — Member C Report

| Field | Details |
|---|---|
| **Owner** | Member C — Sanskar Sharma |
| **Component** | Fixed-size slab allocator |
| **Implementation** | `include/slab_allocator.hpp` |
| **Tests** | `tests/test_slab_allocator.cpp` |

## 1. Purpose

The allocator provides fixed-size storage for objects of type `T` from a pool of compile-time capacity. It is intended for the single-threaded matching-engine path described in the project report and Phase 1 component contract. The pool avoids calling the general-purpose allocator for each slot allocation; it does not yet replace allocations inside the OrderBook.

## 2. Implemented Features

### 2.1 Storage and alignment

`SlabAllocator<T, Capacity>` owns a contiguous `std::byte` array with `Capacity * sizeof(T)` bytes. The array is declared with `alignas(alignof(T))`; each slot begins `sizeof(T)` bytes after the previous one. Since `sizeof(T)` is a multiple of `alignof(T)`, every slot is suitably aligned, including when `T` is over-aligned.

The template requires `Capacity > 0` and `sizeof(T) >= sizeof(void*)`. The latter leaves enough room in a free slot to store its next pointer.

### 2.2 In-place free list

The constructor links the slots in order by storing the next-slot pointer in the first `sizeof(void*)` bytes of each free slot. The last slot stores a null next pointer. `free_head_` identifies the next available slot.

`allocate()` returns `nullptr` when the list is empty. Otherwise it reads the next pointer, removes the head slot, decrements the available count, marks that slot allocated, and returns its storage address. The caller starts the object lifetime with placement `new` before using it as a `T` object.

`deallocate(ptr)` first checks that the address is a slot boundary in this pool, then checks the corresponding slot state. It returns `true` and pushes a live slot onto the free-list head on success. Null, foreign, mid-slot, and already-freed pointers return `false` without changing the list or counters. `owns(ptr)` checks pool range and slot alignment; it does not indicate whether the slot is currently allocated.

The slot index and state checks use integer-address offsets and a per-slot `bool` array. This bookkeeping costs one `bool` per slot and makes invalid frees detectable in O(1). The allocator is non-copyable and non-movable because outstanding pointers refer to its embedded storage.

### 2.3 API and complexity

| API | Behavior | Complexity |
|---|---|---|
| `allocate()` | Returns uninitialized, aligned storage for one `T`, or `nullptr` when full | O(1) |
| `deallocate(T*)` | Returns `true` for a live pool slot; rejects invalid or duplicate frees with `false` | O(1) |
| `owns(const T*)` | Checks whether an address is a slot boundary in this pool | O(1) |
| `available()` | Returns the number of free slots | O(1) |
| `allocated()` | Returns the number of marked allocated slots | O(1) |
| `is_full()` | Reports whether the pool has no free slots | O(1) |
| `capacity()` | Returns the compile-time slot count | O(1) |

Pool construction links all slots in O(`Capacity`) time. Allocation and deallocation do not allocate from the system heap and use no locks.

## 3. Object-Lifetime Contract

The allocator manages storage and slot bookkeeping, not `T` object construction or destruction. Callers must placement-construct a `T` after successful allocation, explicitly destroy it before deallocation, and ensure no live object remains when the pool storage is destroyed. A typical cycle is:

```cpp
T* object = slab.allocate();
if (object != nullptr) {
    new (object) T(/* constructor arguments */);
    // Use object.
    object->~T();
    const bool returned = slab.deallocate(object);
}
```

The allocator is single-threaded and does not synchronize concurrent calls. Its capacity is fixed; exhaustion is reported as `nullptr`, so callers must decide how to handle that condition.

## 4. Unit-Test Coverage

The GoogleTest suite contains 23 tests covering:

- Basic allocation, reads/writes, repeated cycles, and counter consistency.
- Pool exhaustion and recovery after freeing a slot.
- Distinct simultaneous pointers, free-list LIFO reuse, and arbitrary/shuffled free order.
- `owns()` behavior for pool slots, stack and heap pointers, null, and mid-slot pointers.
- Rejection of invalid frees and double frees, with checks that availability and subsequent free-list allocations remain correct.
- Over-aligned slot addresses and explicit constructor/destructor counts.
- Capacity-one and capacity-1024 pools, plus preservation of other live slots during reuse.

## 5. Verified Results

- **Debug build:** 23/23 tests passed using a direct `g++` build with warnings enabled.
- **`-DNDEBUG` build:** 23/23 tests passed using a direct `g++` build with warnings enabled. This exercises invalid-free handling without relying on assertions.
- **ASan/UBSan:** all 23 tests completed successfully. After the test summary, LeakSanitizer reported a fatal environment error and the process exited with status 1; this was not a clean sanitizer process exit.
- **Benchmark:** the current harness compiled and ran directly with optimization enabled. It records elapsed nanoseconds for each 64-operation batch, separately for allocation plus construction and destruction plus release. Allocator order is randomized per round. These results are preliminary and local; they do not establish a repeatable performance improvement.
- **CMake:** configuration and target builds remain unverified because CMake was unavailable in the environment.

## 6. Benchmark Method

The benchmark compares `SlabAllocator<Order>` with `new`/`delete`. Both paths construct an `Order` for each allocation and destroy it on release. Timing is taken around batches of 64 operations to reduce clock-call overhead; the reported percentiles, maximum, and standard deviation describe batch durations, not individual-operation latency. It performs warm-up work and randomizes which allocator runs first each round.

The harness is a microbenchmark for a bounded allocation pattern, not a full matching-engine measurement. Its output recommends repeated runs under controlled conditions. No performance conclusion is made in this report.

## 7. Design Limitations

- The pool has a fixed capacity and supports one object size/type per allocator instance.
- `sizeof(T)` must be at least `sizeof(void*)` for the in-place free-list link.
- Callers own object construction and destruction; incorrect lifetime management remains the caller's responsibility.
- The allocator is not thread-safe and is intended for the project's single-threaded owner path.
- Per-slot allocation tracking adds one `bool` per capacity slot.
- The benchmark does not model OrderBook matching, realistic order lifetimes, or system-wide load, and its measurements are not conclusive performance evidence.

## 8. Pending Integration

The allocator implementation, tests, and standalone benchmark are present on the `slab_allocator` branch. Wiring it into OrderBook ownership and validating the integrated order lifecycle, exhaustion behavior, and matching workload remains pending. No OrderBook integration or integrated performance result is claimed here.
