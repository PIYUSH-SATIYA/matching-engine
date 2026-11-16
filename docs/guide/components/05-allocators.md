# 05 — Allocators and resting-order nodes

Allocation matters when an order becomes a resting book entry. A transient
incoming order is copied into the engine's local `Order in`; it does not require
a V3 `Node` until `book.add(in)` succeeds.

## V1 allocation behaviour

V1 uses standard library containers:

```cpp
std::map<Price, Level>          // tree nodes and price levels
std::list<Order>                // one list node per resting order
std::unordered_map<OrderId,...> // ID index buckets/nodes
```

**Source:** `include/order_book.hpp:64-79`.

V1 has no custom allocator command-line choice.

## V3's allocator is a template parameter

```cpp
using Slab = std::conditional_t<UseHeap,
                                HeapAllocator<Node>,
                                SlabAllocator<Node, MaxOrders>>;
```

**Source:** `include/order_book_fast.hpp:193-195`.

Therefore:

```text
OrderBookFast<..., ..., false> -> SlabAllocator<Node, MaxOrders>
OrderBookFast<..., ..., true>  -> HeapAllocator<Node>
```

## V3 slab allocation

At construction, the slab reserves fixed-size slots and links free slots:

```text
free_head -> slot 0 -> slot 1 -> slot 2 -> ...
```

On allocation:

```cpp
if (free_head_ == nullptr)
    return nullptr;
void* slot = free_head_;
free_head_ = next_pointer_stored_inside(slot);
return reinterpret_cast<T*>(slot);
```

**Source:** `include/slab_allocator.hpp:131-163`.

`OrderBookFast::add()` then starts the `Node` lifetime with placement new:

```cpp
Node* n = slab_->allocate();
if (n == nullptr) return false;
new (n) Node{order, nullptr, nullptr};
```

**Source:** `include/order_book_fast.hpp:109-115`.

On removal, the node is destroyed and its storage is returned. The allocator's
free list is O(1), fixed-capacity, and does not call the general heap per node.

## V3 heap allocation

The heap control uses aligned `operator new` because `Node` is 64-byte aligned:

```cpp
void* p = ::operator new(sizeof(T),
                         std::align_val_t(alignof(T)),
                         std::nothrow);
```

**Source:** `include/order_book_fast.hpp:44-66`.

The book algorithm remains V3. Only node storage changes.

## What the allocator experiment actually compares

```text
V1                  map/list/hash + standard container allocation
V3 fast-slab        array/intrusive-list/flat-hash + fixed slab nodes
V3 fast-heap        array/intrusive-list/flat-hash + aligned heap nodes
```

| Comparison | Changes | Does not change |
|---|---|---|
| V1 vs fast-slab | book structure and allocation | matching rules |
| fast-slab vs fast-heap | node allocation | V3 arrays, links, bitmap, ID hash, matching rules |
| V1 vs fast-heap | book structure and allocation strategy | matching rules |

This is why `fast-heap` is not V2. It is a V3 data structure with a different
node allocator.
