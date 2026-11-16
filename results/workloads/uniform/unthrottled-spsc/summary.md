# Workload results: unthrottled-spsc

Runs: 1. Baseline `results/raw/` was not read or modified.

| profile | runs | p50 (us) | p95 (us) | p99 (us) | p99.9 (us) | throughput | accepted | rejected | parser invalid | generated invalid | duplicates | trades |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| uniform | 1 | 1880881.0 | 32601437.0 | 34856678.0 | 35385664.0 | 5043781 | 2000000 | 64540 | 0 | 0 | 0 | 875043 |

## Charts

![Median latency](plots/latency-p50.svg)

![Tail latency](plots/latency-p999.svg)

![Throughput](plots/throughput.svg)

`invalid` is the server parser count. `generated invalid` and `duplicates` come from the workload manifest; invalid wire records do not enter the book.
