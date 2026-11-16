# Workload results: unthrottled-invalid-spsc

Runs: 1. Baseline `results/raw/` was not read or modified.

| profile | runs | p50 (us) | p95 (us) | p99 (us) | p99.9 (us) | throughput | accepted | rejected | parser invalid | generated invalid | duplicates | trades |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| invalid-mix | 1 | 738560.0 | 29491092.0 | 35342797.0 | 36291397.0 | 4144769 | 1970641 | 9190 | 20169 | 20169 | 19822 | 1292824 |

## Charts

![Median latency](plots/latency-p50.svg)

![Tail latency](plots/latency-p999.svg)

![Throughput](plots/throughput.svg)

`invalid` is the server parser count. `generated invalid` and `duplicates` come from the workload manifest; invalid wire records do not enter the book.
