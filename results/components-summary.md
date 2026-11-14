# Component benchmark summary

Parsed from `/home/pbs/Desktop/projects/os-projects/matching-engine/results/components` by `scripts/analyze_components.py`. Each row is the median across repetitions; the range in brackets is min–max across repetitions.

## Matching core (single thread, no queue, no network)

`bench_order_book ops=2000000 seed=42 reps=9 cpu=2`
`2026-11-14T15:14:31+05:30`

| book | reps | throughput | p50 | p90 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| V1 map+list+unordered_map | 9 | 9.47 M/s | 100 ns | 285 ns | – | 504 ns | 727 ns | 14.56 ms |
| V3 array+slab+bitmap | 9 | 28.95 M/s | 109 ns | 147 ns | – | 278 ns | 467 ns | 26.3 µs |

Latency is per matching operation and includes roughly 25 ns of clock cost in both variants.

Checks: trades=1093640 cancels ok/miss=88304/512136 | V1 == V3 results: YES

## Queue + matching core (two threads, no network)

`bench_pipeline orders=1010000 rate=200000 seed=42 reps=9`
`producer cpu=6 engine cpu=2`
`2026-11-14T15:14:44+05:30`

| queue + book | reps | throughput | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| mutex + V1 | 9 | 2.74 M/s | 1.8 µs | 374.6 µs | 8.02 ms | 12.84 ms |
| mutex + V3 | 9 | 2.94 M/s | 1.8 µs | 5.6 µs | 1.98 ms | 2.82 ms |
| spsc + V1 | 9 | 4.81 M/s | 354 ns | 965 ns | 8.28 ms | 13.03 ms |
| spsc + V3 | 9 | 6.06 M/s | 335 ns | 561 ns | 1.1 µs | 29.4 µs |

Paced at 200k orders/s. This isolates the queue hand-off and the matching loop: no sockets, no wire parsing, no kernel transition.

Checks: result check: trades=789497 traded_qty=20135763  all 4 configs identical: YES

## Allocator in isolation (64-byte nodes)

`bench_allocator ops=2000000 live_set=8192 seed=42 reps=9 cpu=2`
`2026-11-14T15:17:59+05:30`

| allocator | reps | alloc ns/op | alloc p50 | alloc p99 | alloc p99.9 | alloc max | alloc sd | free ns/op | free p50 | free p99.9 | free sd |
|---|---|---|---|---|---|---|---|---|---|---|---|
| heap ::operator new/delete | 9 | 19.77 | 19 ns | 26 ns | 49 ns | 6.1 µs | 21.52 | 25.03 | 22 ns | 110 ns | 19.71 |
| slab (SlabAllocator) | 9 | 14.56 | 14 ns | 16 ns | 19 ns | 6.5 µs | 14.74 | 16.41 | 16 ns | 29 ns | 13.51 |

Slab vs heap: alloc 19.77 → 14.56 ns/op (1.36×), free 25.03 → 16.41 ns/op (1.53×).

Each timed op is bracketed by two `steady_clock` reads and nothing is subtracted, so absolute values are inflated by roughly 20–25 ns. Both variants pay the same clock cost, so the ratio is the meaningful number.

Checks: live_ok=YES ops_ok=YES

## False sharing on the SPSC boundary (padded vs packed indices)

`bench_false_sharing orders=5000000 seed=42 reps=9`
`producer cpu=6 consumer cpu=2`
`2026-11-14T15:18:01+05:30`

| layout | reps | throughput (median) | throughput (best) | throughput range | p50 (median) | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| padded | 9 | 6.95 M/s | 18.11 M/s | 6.84–18.11 M/s | 183 ns | 276 ns | 5.0 µs | 21.9 µs |
| packed | 9 | 8.80 M/s | 16.94 M/s | 6.27–16.94 M/s | 258 ns | 5.8 µs | 30.4 µs | 33.9 µs |

Best-case throughput: padded 18.11 M/s, packed 16.94 M/s → padded/packed = 1.07×.

Why best-case and not the median: on this desktop host a spin-waiting pair of threads is preempted at unpredictable moments, and preemption can only *subtract* throughput. The medians therefore overlap even when the layouts differ, while the fastest of N identical trials estimates what the layout can do when the host leaves it alone. Both layouts get the same number of trials, so the comparison is fair, but it is an upper-bound comparison, not a typical-case one.

Per-repetition ratio lines: last observed `padded_vs_packed_throughput=0.52 x`. A value near 1.0 means the padded and packed layouts performed the same on this host.

