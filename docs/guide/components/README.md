# Component-by-component study guide

This folder is the implementation-first companion to the lifecycle guide. Read
the files in this order:

1. [00 — the missing V2 and the naming map](00-version-map.md)
2. [01 — order and wire format](01-order-and-wire.md)
3. [02 — network layer](02-network-layer.md)
4. [03 — ring buffers](03-ring-buffers.md)
5. [04 — order books](04-order-books.md)
6. [05 — allocators and nodes](05-allocators.md)
7. [06 — matching engine](06-matching-engine.md)
8. [07 — sinks, events, and measurements](07-sinks-and-measurements.md)
9. [08 — wiring and experiment combinations](08-wiring-and-experiments.md)

These are not abstract designs. Each file explains the implementation currently
in this repository and cites the relevant source file and line range.

## The one-sentence model

```text
client bytes -> network parser -> Order -> queue -> matching engine
             -> existing opposite-side book orders
             -> trades + book mutation -> sink/statistics
```

The most important correction to keep in mind is:

> An incoming order is **not inserted into the book before matching**. The
> engine keeps it in a local variable, looks up resting orders on the opposite
> side, trades against them, and inserts only a remaining quantity.
