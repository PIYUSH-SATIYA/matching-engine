# Component benchmark summary

Parsed from `/home/pbs/Desktop/projects/os-projects/matching-engine/results/components` by `scripts/analyze_components.py`. Each row is the median across repetitions; the range in brackets is min–max across repetitions.

## Matching core (single thread, no queue, no network)

`bench_order_book ops=2000000 seed=42 reps=9 cpu=2`
`2026-11-14T18:28:27+05:30`

| book | reps | throughput | p50 | p90 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| V1 map+list+unordered_map | 9 | 11.63 M/s | 81 ns | 226 ns | – | 420 ns | 595 ns | 10.97 ms |
| V3 array+slab+bitmap | 9 | 33.68 M/s | 94 ns | 126 ns | – | 247 ns | 374 ns | 26.7 µs |

Latency is per matching operation and includes roughly 25 ns of clock cost in both variants.

Checks: trades=1093640 cancels ok/miss=88304/512136 | V1 == V3 results: YES

## Queue + matching core (two threads, no network)

`bench_pipeline orders=1010000 rate=200000 seed=42 reps=9`
`producer cpu=6 engine cpu=2`
`2026-11-14T18:28:38+05:30`

| queue + book | reps | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| mutex + V1 | 9 | 3.27 M/s | 1.6 µs | 3.6 µs | 6.26 ms | 11.02 ms |
| mutex + V3 | 9 | 3.49 M/s | 1.6 µs | 3.9 µs | 14.6 µs | 404.3 µs |
| spsc + V1 | 9 | 5.77 M/s | 247 ns | 836 ns | 5.40 ms | 10.24 ms |
| spsc + V3 | 9 | 7.35 M/s | 299 ns | 1.6 µs | 13.2 µs | 160.2 µs |

Paced at 200k orders/s. This isolates the queue hand-off and the matching loop: no sockets, no wire parsing, no kernel transition.

Checks: result check: trades=789497 traded_qty=20135763  all 4 configs identical: YES

## Allocator in isolation (64-byte nodes)

`bench_allocator ops=2000000 live_set=8192 seed=42 reps=9 cpu=2`
`2026-11-14T18:31:52+05:30`

| allocator | reps | alloc ns/op | alloc p50 | alloc p99 | alloc p99.9 | alloc max | alloc sd | free ns/op | free p50 | free p99.9 | free sd |
|---|---|---|---|---|---|---|---|---|---|---|---|
| heap ::operator new/delete | 9 | 17.14 | 17 ns | 24 ns | 44 ns | 15.3 µs | 27.50 | 22.19 | 19 ns | 136 ns | 86.17 |
| slab (SlabAllocator) | 9 | 12.59 | 13 ns | 14 ns | 16 ns | 3.1 µs | 6.93 | 14.13 | 14 ns | 24 ns | 8.47 |

Slab vs heap: alloc 17.14 → 12.59 ns/op (1.36×), free 22.19 → 14.13 ns/op (1.57×).

Each timed op is bracketed by two `steady_clock` reads and nothing is subtracted, so absolute values are inflated by roughly 20–25 ns. Both variants pay the same clock cost, so the ratio is the meaningful number.

Checks: live_ok=YES ops_ok=YES

## False sharing on the SPSC boundary (padded vs packed indices)

`bench_false_sharing orders=5000000 seed=42 reps=9`
`producer cpu=6 consumer cpu=2`
`2026-11-14T18:31:53+05:30`

| layout | reps | throughput (median) | throughput (best) | throughput range | p50 (median) | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| padded | 9 | 8.59 M/s | 9.66 M/s | 8.17–9.66 M/s | 153 ns | 244 ns | 17.8 µs | 70.3 µs |
| packed | 9 | 10.28 M/s | 11.78 M/s | 7.99–11.78 M/s | 197 ns | 530 ns | 26.2 µs | 71.8 µs |

Best-case throughput: padded 9.66 M/s, packed 11.78 M/s → padded/packed = 0.82×.

Why best-case and not the median: on this desktop host a spin-waiting pair of threads is preempted at unpredictable moments, and preemption can only *subtract* throughput. The medians therefore overlap even when the layouts differ, while the fastest of N identical trials estimates what the layout can do when the host leaves it alone. Both layouts get the same number of trials, so the comparison is fair, but it is an upper-bound comparison, not a typical-case one.

Per-repetition ratio lines: last observed `padded_vs_packed_throughput=0.79 x`. A value near 1.0 means the padded and packed layouts performed the same on this host.

