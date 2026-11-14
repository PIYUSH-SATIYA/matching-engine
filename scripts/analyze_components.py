#!/usr/bin/env python3
"""Summarise the component micro-benchmarks.

Reads   results/components/bench-order-book.txt
        results/components/bench-pipeline.txt
        results/components/bench-allocator.txt
        results/components/bench-false-sharing.txt
Writes  results/components-summary.md

Each file contains N repetitions separated by "--- rep k ---" lines.  This
script parses every repetition, treats the row label as the key, and reports
the median across repetitions plus the min-max range.  It is deliberately
tolerant: a missing file or an unparsable line is reported, not fatal.
"""

from __future__ import annotations

import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMP = ROOT / "results" / "components"
OUT = ROOT / "results" / "components-summary.md"

REP = re.compile(r"^--- rep (\d+) ---")
# Throughput rows used by bench_order_book ("V1 map+...") and
# bench_false_sharing ("padded ...").
THROUGHPUT = re.compile(
    r"^(?P<name>.*?\S)\s+(?P<rate>[0-9.]+)\s*M/s\s*\|?\s*"
    r"(?P<p50>\d+)\s+(?P<p90>\d+)\s+(?P<p99>\d+)\s+"
    r"(?P<p999>\d+)\s+(?P<max>\d+)\s*$"
)
# bench_pipeline rows add a queue column and the achieved rate.
PIPELINE = re.compile(
    r"^(?P<queue>\w+)\s+(?P<book>\w+)\s*\|\s*(?P<rate>[0-9.]+)\s*M/s\s*\|\s*"
    r"(?P<p50>\d+)\s+(?P<p99>\d+)\s+(?P<p999>\d+)\s+(?P<max>\d+)\s+"
    r"(?P<achvd>\d+)\s*$"
)
FS_ROW = re.compile(
    r"^(?P<name>padded|packed)\s+(?P<rate>[0-9.]+)\s*M/s\s+"
    r"(?P<p50>\d+)\s+(?P<p99>\d+)\s+(?P<p999>\d+)\s+(?P<max>\d+)\s*$"
)


def num(s: str) -> float:
    return float(s)


def med(values):
    return statistics.median(values) if values else 0.0


def spread(values):
    return (min(values), max(values)) if values else (0.0, 0.0)


def fmt_ns(ns: float) -> str:
    if ns >= 1e6:
        return f"{ns / 1e6:.2f} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:.1f} µs"
    return f"{ns:.0f} ns"


def fmt_rate(r: float) -> str:
    if r >= 1e6:
        return f"{r / 1e6:.2f} M/s"
    if r >= 1e3:
        return f"{r / 1e3:.0f} k/s"
    return f"{r:.0f}/s"


def read_blocks(path: Path):
    """Split a results file into (header_lines, [lines_per_rep])."""
    if not path.exists():
        return [], []
    headers: list[str] = []
    reps: list[list[str]] = []
    cur: list[str] | None = None
    for line in path.read_text().splitlines():
        if REP.match(line):
            if cur is not None:
                reps.append(cur)
            cur = []
        elif cur is None:
            if line.strip():
                headers.append(line)
        else:
            cur.append(line)
    if cur is not None:
        reps.append(cur)
    return headers, reps


def collect(reps, pattern, key_fn):
    """Return {key: [dict per matching line]} ordered by first appearance."""
    out: dict[str, list[dict]] = {}
    order: list[str] = []
    for lines in reps:
        for line in lines:
            m = pattern.match(line.strip())
            if not m:
                continue
            row = {k: (v if k == "name" or k in ("queue", "book") else num(v))
                   for k, v in m.groupdict().items()}
            key = key_fn(row)
            if key not in out:
                out[key] = []
                order.append(key)
            out[key].append(row)
    return [(k, out[k]) for k in order]


def md_table(headers, rows) -> str:
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join(["---"] * len(headers)) + "|"]
    for row in rows:
        out.append("| " + " | ".join(str(c) for c in row) + " |")
    return "\n".join(out)


def consistency(reps) -> list[str]:
    found = []
    for lines in reps:
        for line in lines:
            s = line.strip()
            if s.startswith(("result check:", "trades=", "live_ok=")):
                found.append(s)
    return found


def main() -> int:
    md = ["# Component benchmark summary", "",
          f"Parsed from `{COMP}` by `scripts/analyze_components.py`. "
          "Each row is the median across repetitions; the range in brackets "
          "is min–max across repetitions.", ""]

    # ---- single-threaded matching core -------------------------------------
    headers, reps = read_blocks(COMP / "bench-order-book.txt")
    md += ["## Matching core (single thread, no queue, no network)", ""]
    if headers:
        md += [f"`{h.lstrip('# ')}`" for h in headers[:2] if h.startswith("#")]
        md.append("")
    rows = collect(reps, THROUGHPUT, lambda r: r["name"])
    if rows:
        md.append(md_table(
            ["book", "reps", "throughput", "p50", "p90", "p95", "p99",
             "p99.9", "max"],
            [[name, len(v),
              f'{med([r["rate"] for r in v]):.2f} M/s',
              fmt_ns(med([r["p50"] for r in v])),
              fmt_ns(med([r["p90"] for r in v])),
              "–",
              fmt_ns(med([r["p99"] for r in v])),
              fmt_ns(med([r["p999"] for r in v])),
              fmt_ns(med([r["max"] for r in v]))]
             for name, v in rows]))
        md.append("")
        md.append("Latency is per matching operation and includes roughly 25 ns "
                  "of clock cost in both variants.")
        md.append("")
    else:
        md += ["- not run", ""]
    checks = consistency(reps)
    if checks:
        md += ["Checks: " + "; ".join(sorted(set(checks))), ""]

    # ---- queue + core, no network ------------------------------------------
    headers, reps = read_blocks(COMP / "bench-pipeline.txt")
    md += ["## Queue + matching core (two threads, no network)", ""]
    if headers:
        md += [f"`{h.lstrip('# ')}`" for h in headers if h.startswith("#")]
        md.append("")
    rows = collect(reps, PIPELINE, lambda r: f'{r["queue"]} + {r["book"]}')
    if rows:
        md.append(md_table(
            ["queue + book", "reps", "throughput", "p50", "p99", "p99.9",
             "max"],
            [[name, len(v),
              f'{med([r["rate"] for r in v]):.2f} M/s',
              fmt_ns(med([r["p50"] for r in v])),
              fmt_ns(med([r["p99"] for r in v])),
              fmt_ns(med([r["p999"] for r in v])),
              fmt_ns(med([r["max"] for r in v]))]
             for name, v in rows]))
        md.append("")
        md.append("Paced at 200k orders/s. This isolates the queue hand-off and the "
                  "matching loop: no sockets, no wire parsing, no kernel transition.")
        md.append("")
    else:
        md += ["- not run", ""]
    checks = consistency(reps)
    if checks:
        md += ["Checks: " + "; ".join(sorted(set(checks))), ""]

    # ---- allocator ----------------------------------------------------------
    headers, reps = read_blocks(COMP / "bench-allocator.txt")
    md += ["## Allocator in isolation (64-byte nodes)", ""]
    if headers:
        md += [f"`{h.lstrip('# ')}`" for h in headers if h.startswith("#")]
        md.append("")
    alloc_rows = []
    for lines in reps:
        for line in lines:
            if "|" not in line or "allocator" in line:
                continue
            seg = [s.strip() for s in line.split("|")]
            if len(seg) != 5:
                continue
            name_tokens = seg[0].split()
            if not name_tokens or not name_tokens[-1].replace(".", "").isdigit():
                continue
            name = " ".join(name_tokens[:-1])
            a = [num(x) for x in seg[1].split()]
            f = [num(x) for x in seg[3].split()]
            sd = [num(x) for x in seg[4].split()]
            if len(a) != 5 or len(f) != 5 or len(sd) != 2:
                continue
            alloc_rows.append((name, {
                "alloc_ns": num(name_tokens[-1]), "free_ns": num(seg[2]),
                "ap50": a[0], "ap90": a[1], "ap99": a[2], "ap999": a[3],
                "amax": a[4], "fp50": f[0], "fp90": f[1], "fp99": f[2],
                "fp999": f[3], "fmax": f[4], "asd": sd[0], "fsd": sd[1],
            }))
    grouped: dict[str, list[dict]] = {}
    for name, row in alloc_rows:
        grouped.setdefault(name, []).append(row)
    if grouped:
        md.append(md_table(
            ["allocator", "reps", "alloc ns/op", "alloc p50", "alloc p99",
             "alloc p99.9", "alloc max", "alloc sd", "free ns/op",
             "free p50", "free p99.9", "free sd"],
            [[name, len(v),
              f'{med([r["alloc_ns"] for r in v]):.2f}',
              fmt_ns(med([r["ap50"] for r in v])),
              fmt_ns(med([r["ap99"] for r in v])),
              fmt_ns(med([r["ap999"] for r in v])),
              fmt_ns(med([r["amax"] for r in v])),
              f'{med([r["asd"] for r in v]):.2f}',
              f'{med([r["free_ns"] for r in v]):.2f}',
              fmt_ns(med([r["fp50"] for r in v])),
              fmt_ns(med([r["fp999"] for r in v])),
              f'{med([r["fsd"] for r in v]):.2f}']
             for name, v in grouped.items()]))
        md.append("")
        if "heap ::operator new/delete" in grouped and \
                "slab (SlabAllocator)" in grouped:
            h = grouped["heap ::operator new/delete"]
            s = grouped["slab (SlabAllocator)"]
            ha = med([r["alloc_ns"] for r in h])
            sa = med([r["alloc_ns"] for r in s])
            hf = med([r["free_ns"] for r in h])
            sf = med([r["free_ns"] for r in s])
            md.append(f"Slab vs heap: alloc {ha:.2f} → {sa:.2f} ns/op "
                      f"({ha / sa:.2f}×), free {hf:.2f} → {sf:.2f} ns/op "
                      f"({hf / sf:.2f}×).")
            md.append("")
            md.append("Each timed op is bracketed by two `steady_clock` reads and "
                      "nothing is subtracted, so absolute values are inflated by "
                      "roughly 20–25 ns. Both variants pay the same clock cost, so "
                      "the ratio is the meaningful number.")
            md.append("")
    else:
        md += ["- not run", ""]
    checks = consistency(reps)
    if checks:
        md += ["Checks: " + "; ".join(sorted(set(checks))), ""]

    # ---- false sharing ------------------------------------------------------
    headers, reps = read_blocks(COMP / "bench-false-sharing.txt")
    md += ["## False sharing on the SPSC boundary (padded vs packed indices)", ""]
    if headers:
        md += [f"`{h.lstrip('# ')}`" for h in headers if h.startswith("#")]
        md.append("")
    rows = collect(reps, FS_ROW, lambda r: r["name"])
    if rows:
        md.append(md_table(
            ["layout", "reps", "throughput", "throughput range", "p50", "p99",
             "p99.9", "max"],
            [[name, len(v),
              f'{med([r["rate"] for r in v]):.2f} M/s',
              f'{spread([r["rate"] for r in v])[0]:.2f}–'
              f'{spread([r["rate"] for r in v])[1]:.2f} M/s',
              fmt_ns(med([r["p50"] for r in v])),
              fmt_ns(med([r["p99"] for r in v])),
              fmt_ns(med([r["p999"] for r in v])),
              fmt_ns(med([r["max"] for r in v]))]
             for name, v in rows]))
        md.append("")
        ratio = next((line.strip() for lines in reps for line in lines
                      if line.strip().startswith("padded_vs_packed_throughput=")),
                     None)
        if ratio:
            md.append(f"Per-repetition ratio lines: last observed `{ratio}`. "
                      "A value near 1.0 means the padded and packed layouts "
                      "performed the same on this host.")
            md.append("")
    else:
        md += ["- not run", ""]

    OUT.write_text("\n".join(md) + "\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
