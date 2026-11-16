# 00 — Version map: why V1 and V3, where is V2?

## Short answer

There is no implemented V2 in this repository.

The code has:

| Name in the project | Actual implementation |
|---|---|
| V1 / `OrderBook` | reference book using `std::map`, `std::list`, and `std::unordered_map` |
| V2 | not present; no `OrderBookV2`, no `v2` command-line option |
| V3 / `OrderBookFast` | optimized book using direct arrays, intrusive lists, bitmap metadata, flat hash, and configurable node allocator |

The name V3 is historical: the project went through design iterations and the
final optimized design was called “V3”. Version numbers are not a promise that
every intermediate version still exists as a selectable runtime component.

The source confirms the available choices in `src/main.cpp`:

```cpp
if (opt.book == "v1")
    return runApp<Queue, OrderBook>(opt);
if (opt.book == "fast-heap")
    return runApp<Queue, OrderBookFast<..., ..., true>>(opt);
return runApp<Queue, OrderBookFast<...>>(opt); // fast-slab
```

**Source:** `src/main.cpp:446-451`.

The help text exposes exactly these names:

```text
--book v1|fast-slab|fast-heap
```

**Source:** `src/main.cpp:454-473`.

## V1 is a book version, not an allocator version

This distinction caused much of the confusion:

```text
V1 = one complete order-book data-structure design
V3 = another complete order-book data-structure design

fast-slab = V3 + SlabAllocator
fast-heap = V3 + HeapAllocator
```

So the real tree is:

```text
book choice
├── v1
│   ├── std::map price levels
│   ├── std::list orders at each price
│   └── std::unordered_map order-id -> list iterator
└── V3 / OrderBookFast
    ├── fast-slab: V3 nodes from SlabAllocator
    └── fast-heap: V3 nodes from HeapAllocator
```

`fast-heap` is not “V2”. It is an allocator control experiment: the V3 book
algorithm stays the same while node allocation changes.

## What would V2 have been?

Do not invent a V2 implementation when explaining this project. The repository
does not define one, and no benchmark measures one. If you need a historical
sentence for a presentation, use:

> V1 is the simple reference design; intermediate V2 work was not retained as a
> public implementation; V3 is the final optimized design. The measured runtime
> choices are V1, V3 with a slab, and V3 with heap allocation.
