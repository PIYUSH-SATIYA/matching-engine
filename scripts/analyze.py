#!/usr/bin/env python3
"""Analyse matching-engine benchmark runs using only the Python standard library.

Reads   results/raw/<config>/runs.csv        (one row per run)
        results/raw/<config>/samples-run1.bin (u64 count + u64 latency ns)
Writes  results/summary.csv
        results/summary.md
        results/analysis.json
        results/plots/*.svg
"""

from __future__ import annotations

import csv
import json
import math
import os
import statistics
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = Path(os.environ.get("RESULTS", ROOT / "results"))
RAW = RESULTS / "raw"
PLOTS = RESULTS / "plots"

NUMERIC = [
    "run", "orders", "warmup", "measured", "accepted", "trades", "rejected",
    "invalid", "duration_s", "throughput", "p50", "p90", "p99", "p999", "max",
    "minflt", "majflt", "nvcsw", "nivcsw", "user_s", "sys_s", "migrations",
    "kernel_ops", "traded_qty", "traded_notional", "net_nvcsw", "net_nivcsw",
    "net_user_s", "net_sys_s", "spsc_yields",
]


def load_rows() -> list[dict]:
    rows = []
    for path in sorted(RAW.glob("*/runs.csv")):
        with path.open(newline="") as fh:
            for row in csv.DictReader(fh):
                if not row.get("config"):
                    continue
                for key in NUMERIC:
                    try:
                        row[key] = float(row[key])
                    except (KeyError, TypeError, ValueError):
                        row[key] = 0.0
                row["run"] = int(row["run"])
                rows.append(row)
    return rows


def median(values):
    return float(statistics.median(values)) if values else 0.0


def summary_for(rows: list[dict]) -> dict:
    def col(key):
        return [r[key] for r in rows]

    measured = median(col("measured"))
    cpu_s = median([r["user_s"] + r["sys_s"] for r in rows])
    net_cpu_s = median([r["net_user_s"] + r["net_sys_s"] for r in rows])
    return {
        "runs": len(rows),
        "orders": median(col("orders")),
        "measured": measured,
        "throughput": median(col("throughput")),
        "p50": median(col("p50")),
        "p90": median(col("p90")),
        "p99": median(col("p99")),
        "p999": median(col("p999")),
        "max": median(col("max")),
        "p999_min": min(col("p999")) if rows else 0,
        "p999_max": max(col("p999")) if rows else 0,
        "trades": median(col("trades")),
        "nvcsw": median(col("nvcsw")),
        "nivcsw": median(col("nivcsw")),
        "kernel_ops": median(col("kernel_ops")),
        "user_s": median(col("user_s")),
        "sys_s": median(col("sys_s")),
        "cpu_us_per_order": (cpu_s / measured * 1e6) if measured else 0.0,
        "net_nvcsw": median(col("net_nvcsw")),
        "net_nivcsw": median(col("net_nivcsw")),
        "net_cpu_us_per_order": (net_cpu_s / measured * 1e6) if measured else 0.0,
        "spsc_yields": median(col("spsc_yields")),
        "traded_qty": median(col("traded_qty")),
        "traded_notional": median(col("traded_notional")),
        "accepted": median(col("accepted")),
        "rejected": median(col("rejected")),
        "invalid": median(col("invalid")),
    }


def short_name(config: str) -> str:
    if config.startswith("q-"):
        parts = config[2:].split("_")
        q = parts[0].replace("q-", "").replace("queue-", "")
        sched = parts[1].replace("sched-", "")
        net = parts[2].replace("net-", "")
        return f"{q} / {sched} / {net}"
    if config.startswith("alloc-"):
        return "allocator: " + config[len("alloc-"):]
    return config


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


def md_table(headers: list[str], rows: list[list[str]]) -> str:
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join(["---"] * len(headers)) + "|"]
    for row in rows:
        out.append("| " + " | ".join(str(c) for c in row) + " |")
    return "\n".join(out)


def read_samples(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) < 8:
        return []
    (n,) = struct.unpack_from("<Q", data, 0)
    n = min(n, (len(data) - 8) // 8)
    return list(struct.unpack_from("<" + "Q" * n, data, 8))


def cdf_points(samples: list[int], points: int = 2000):
    if not samples:
        return []
    values = sorted(samples)
    n = len(values)
    pts = []
    for i in range(points + 1):
        q = i / points
        idx = min(n - 1, int(q * (n - 1)))
        pts.append((max(1.0, float(values[idx])), q * 100.0))
    return pts


def svg_cdf(title: str, series: list[tuple[str, list[int]]], out: Path) -> None:
    series = [(name, cdf_points(s)) for name, s in series if s]
    if not series:
        return
    width, height = 980, 560
    left, right, top, bottom = 90, 250, 45, 70
    pw, ph = width - left - right, height - top - bottom
    all_x = [x for _, pts in series for x, _ in pts]
    xmin = max(1.0, min(all_x))
    xmax = max(all_x)
    if xmax <= xmin:
        xmax = xmin * 10
    lxmin, lxmax = math.log10(xmin), math.log10(xmax)

    def sx(x):
        return left + (math.log10(max(x, xmin)) - lxmin) / (lxmax - lxmin) * pw

    def sy(y):
        return top + ph - (y / 100.0) * ph

    colors = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#17becf"]
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
             f'viewBox="0 0 {width} {height}" font-family="sans-serif">',
             f'<rect width="{width}" height="{height}" fill="white"/>',
             f'<text x="{left}" y="26" font-size="20" font-weight="bold">{title}</text>']
    # grid + y labels
    for pct in (0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100):
        y = sy(pct)
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + pw}" y2="{y:.1f}" '
                     f'stroke="#e5e5e5" stroke-width="1"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 4:.1f}" font-size="12" '
                     f'text-anchor="end">{pct}%</text>')
    # x ticks at powers of ten
    p = math.ceil(lxmin)
    while p <= lxmax + 1e-9:
        x = left + (p - lxmin) / (lxmax - lxmin) * pw
        label = fmt_ns(10 ** p)
        parts.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top + ph}" '
                     f'stroke="#f0f0f0" stroke-width="1"/>')
        parts.append(f'<text x="{x:.1f}" y="{top + ph + 22}" font-size="12" '
                     f'text-anchor="middle">{label}</text>')
        p += 1
    # axes
    parts.append(f'<line x1="{left}" y1="{top + ph}" x2="{left + pw}" y2="{top + ph}" stroke="#333"/>')
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + ph}" stroke="#333"/>')
    parts.append(f'<text x="{left + pw / 2}" y="{height - 18}" font-size="14" '
                 f'text-anchor="middle">client-send → engine-complete latency (log scale)</text>')
    parts.append(f'<text transform="translate(24 {top + ph / 2}) rotate(-90)" '
                 f'font-size="14" text-anchor="middle">cumulative share of orders</text>')
    # lines + legend
    for i, (name, pts) in enumerate(series):
        color = colors[i % len(colors)]
        d = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in pts)
        parts.append(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="2"/>')
    ly = top + 10
    for i, (name, _) in enumerate(series):
        color = colors[i % len(colors)]
        parts.append(f'<line x1="{left + pw + 25}" y1="{ly - 4}" x2="{left + pw + 55}" '
                     f'y2="{ly - 4}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<text x="{left + pw + 62}" y="{ly}" font-size="13">{name}</text>')
        ly += 24
    parts.append("</svg>")
    out.write_text("\n".join(parts))


def svg_hbars(title: str, items: list[tuple[str, float]], out: Path,
              fmt, note: str) -> None:
    if not items:
        return
    items = sorted(items, key=lambda kv: kv[1], reverse=True)
    row_h = 30
    width = 980
    height = 90 + row_h * len(items)
    left, right, top = 260, 120, 55
    pw = width - left - right
    vmax = max(v for _, v in items) or 1.0
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
             f'viewBox="0 0 {width} {height}" font-family="sans-serif">',
             f'<rect width="{width}" height="{height}" fill="white"/>',
             f'<text x="24" y="30" font-size="20" font-weight="bold">{title}</text>']
    for i, (name, value) in enumerate(items):
        y = top + i * row_h
        w = max(1.0, value / vmax * pw)
        color = "#1f77b4" if i else "#d62728"
        parts.append(f'<rect x="{left}" y="{y}" width="{w:.1f}" height="{row_h - 10}" '
                     f'fill="{color}" opacity="0.85"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 15}" font-size="13" '
                     f'text-anchor="end">{name}</text>')
        parts.append(f'<text x="{left + w + 8:.1f}" y="{y + 15}" font-size="12">{fmt(value)}</text>')
    parts.append(f'<text x="24" y="{height - 18}" font-size="12" fill="#555">{note}</text>')
    parts.append("</svg>")
    out.write_text("\n".join(parts))


def config_samples(config: str) -> list[int]:
    path = RAW / config / "samples-run1.bin"
    return read_samples(path) if path.exists() else []


def main() -> int:
    rows = load_rows()
    if not rows:
        print(f"no rows under {RAW}", file=sys.stderr)
        return 1
    PLOTS.mkdir(parents=True, exist_ok=True)

    groups = defaultdict(list)
    for row in rows:
        groups[(row["config"], row["kind"])].append(row)
    summaries = {f"{c}|{k}": summary_for(v) for (c, k), v in groups.items()}

    # ---- summary.csv -------------------------------------------------------
    fields = ["config", "kind", "runs", "measured", "throughput", "p50", "p90",
              "p99", "p999", "p999_min", "p999_max", "max", "p999_over_p50",
              "cpu_us_per_order", "net_cpu_us_per_order", "nvcsw", "nivcsw",
              "net_nvcsw", "net_nivcsw", "kernel_ops", "spsc_yields", "user_s",
              "sys_s", "accepted", "rejected", "invalid", "trades",
              "traded_qty", "traded_notional"]
    with (RESULTS / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for (config, kind) in sorted(groups):
            s = summaries[f"{config}|{kind}"]
            row = {"config": config, "kind": kind, **s}
            row["p999_over_p50"] = (s["p999"] / s["p50"]) if s["p50"] else 0.0
            writer.writerow({k: row[k] for k in fields})

    latency = [(c, s) for (c, k), s in summaries.items() if k == "latency"]
    throughput = [(c, s) for (c, k), s in summaries.items() if k == "throughput"]

    # ---- outlier flags -----------------------------------------------------
    flags = []
    for (config, kind), runs in sorted(groups.items()):
        if kind != "latency" or len(runs) < 3:
            continue
        med = median([r["p999"] for r in runs])
        for r in runs:
            if med > 0 and r["p999"] > 1.2 * med:
                flags.append((config, r["run"], r["p999"], med))

    # ---- checksums ---------------------------------------------------------
    checksum_groups = defaultdict(set)
    for row in rows:
        checksum_groups[row["kind"]].add(
            (int(row["orders"]), int(row["warmup"]), int(row["trades"]),
             int(row["traded_qty"]), int(row["traded_notional"]))
        )

    # ---- markdown ----------------------------------------------------------
    md = ["# Benchmark summary", ""]
    md.append(f"Generated by `scripts/analyze.py` from `{RAW}`.")
    md.append("Latency is client-send → engine-complete (CLOCK_MONOTONIC, same host). "
              "Latency runs use an open-loop 200k orders/s client; throughput runs are "
              "unthrottled. Values are medians across repetitions; the p99.9 range shows "
              "the spread across runs.")
    md.append("")
    md.append("## Latency runs (200k orders/s, 10k warmup)")
    md.append("")
    md.append(md_table(
        ["config", "runs", "p50", "p90", "p99", "p99.9", "p99.9 range", "max",
         "p99.9/p50", "kernel ops/run"],
        [[short_name(c), s["runs"], fmt_ns(s["p50"]), fmt_ns(s["p90"]),
          fmt_ns(s["p99"]), fmt_ns(s["p999"]),
          f'{fmt_ns(s["p999_min"])}–{fmt_ns(s["p999_max"])}',
          fmt_ns(s["max"]), f'{s["p999"] / s["p50"]:.0f}×' if s["p50"] else "-",
          f'{s["kernel_ops"]:.0f}'] for c, s in sorted(latency)]))
    md.append("")
    md.append("## Throughput runs (unthrottled)")
    md.append("")
    md.append(md_table(
        ["config", "runs", "orders/s", "engine CPU/order", "net CPU/order",
         "nvcsw", "nivcsw", "net nvcsw", "kernel ops/run", "SPSC yields"],
        [[short_name(c), s["runs"], fmt_rate(s["throughput"]),
          f'{s["cpu_us_per_order"]:.2f} µs',
          f'{s["net_cpu_us_per_order"]:.2f} µs', f'{s["nvcsw"]:.0f}',
          f'{s["nivcsw"]:.0f}', f'{s["net_nvcsw"]:.0f}',
          f'{s["kernel_ops"]:.0f}', f'{s["spsc_yields"]:.0f}']
         for c, s in sorted(throughput, key=lambda kv: -kv[1]["throughput"])]))
    md.append("")
    md.append("## Component views")
    md.append("")
    md.append("Queue (fixed: fast-slab, epoll, pinned engine/network/client):")
    md.append("")
    qrows = []
    for c, s in latency:
        if c.startswith("q-") and "sched-pin" in c and "net-epoll" in c:
            qrows.append([short_name(c), fmt_ns(s["p50"]), fmt_ns(s["p99"]),
                          fmt_ns(s["p999"]), f'{s["p999"] / s["p50"]:.0f}×'])
    md.append(md_table(["config", "p50", "p99", "p99.9", "p99.9/p50"], qrows))
    md.append("")
    md.append("Allocator (fixed: spsc, epoll, pinned):")
    md.append("")
    arows = [[short_name(c), fmt_ns(s["p50"]), fmt_ns(s["p99"]), fmt_ns(s["p999"]),
              fmt_rate(s["throughput"])] for c, s in latency if c.startswith("alloc-")]
    md.append(md_table(["config", "p50", "p99", "p99.9", "latency-run rate"], arows))
    md.append("")
    md.append("Scheduling (fixed: spsc, fast-slab, epoll):")
    md.append("")
    srows = [[short_name(c), fmt_ns(s["p50"]), fmt_ns(s["p99"]), fmt_ns(s["p999"]),
              f'{s["nivcsw"]:.0f}', f'{s["nvcsw"]:.0f}']
             for c, s in latency
             if c.startswith("q-spsc") and "net-epoll" in c]
    md.append(md_table(["config", "p50", "p99", "p99.9", "involuntary switches", "voluntary switches"], srows))
    md.append("")
    md.append("Network (fixed: spsc, fast-slab, pinned):")
    md.append("")
    nrows = [[short_name(c), fmt_ns(s["p50"]), fmt_ns(s["p99"]), fmt_ns(s["p999"]),
              f'{s["kernel_ops"]:.0f}']
             for c, s in latency if c.startswith("q-spsc") and "sched-pin" in c]
    md.append(md_table(["config", "p50", "p99", "p99.9", "network kernel ops/run"], nrows))
    md.append("")
    md.append("## Correctness checksums")
    md.append("")
    md.append("Identically seeded runs must produce the same trade count, traded quantity "
              "and traded notional across queue, allocator, scheduling and network variants.")
    md.append("")
    for kind, tuples in sorted(checksum_groups.items()):
        md.append(f"- `{kind}`: {len(tuples)} distinct checksum(s) across all configs.")
        for t in sorted(tuples):
            md.append(f"  - orders={t[0]} warmup={t[1]} trades={t[2]} qty={t[3]} notional={t[4]}")
    md.append("")
    md.append("## Flagged runs (p99.9 > 1.2× config median)")
    md.append("")
    if flags:
        for config, run, value, med in flags:
            md.append(f"- `{config}` run {run}: p99.9={fmt_ns(value)} vs median {fmt_ns(med)}")
    else:
        md.append("- none")
    md.append("")
    (RESULTS / "summary.md").write_text("\n".join(md) + "\n")

    # ---- plots -------------------------------------------------------------
    comparisons = {
        "cdf-queue": [("spsc", "q-spsc_sched-pin_net-epoll"),
                      ("mutex", "q-mutex_sched-pin_net-epoll")],
        "cdf-allocator": [("slab", "alloc-fast-slab"),
                          ("heap", "alloc-fast-heap"),
                          ("v1 map/list", "alloc-v1")],
        "cdf-scheduling": [("default", "q-spsc_sched-default_net-epoll"),
                           ("pinned", "q-spsc_sched-pin_net-epoll")],
        "cdf-network": [("epoll", "q-spsc_sched-pin_net-epoll"),
                        ("io_uring", "q-spsc_sched-pin_net-uring")],
        "cdf-matrix-spsc": [("epoll default", "q-spsc_sched-default_net-epoll"),
                            ("uring default", "q-spsc_sched-default_net-uring"),
                            ("epoll pinned", "q-spsc_sched-pin_net-epoll"),
                            ("uring pinned", "q-spsc_sched-pin_net-uring")],
    }
    for name, configs in comparisons.items():
        series = [(label, config_samples(config)) for label, config in configs]
        if any(s for _, s in series):
            svg_cdf(name.replace("cdf-", "Latency CDF: ") + " (run 1)",
                    series, PLOTS / f"{name}.svg")

    thr_items = [(short_name(c), s["throughput"]) for c, s in throughput]
    svg_hbars("Throughput by configuration (median)", thr_items,
              PLOTS / "bar-throughput.svg", fmt_rate,
              "Unthrottled client, 2M orders, fast-slab unless labelled otherwise.")
    lat_items = [(short_name(c), s["p999"]) for c, s in latency]
    svg_hbars("p99.9 latency by configuration (median)", lat_items,
              PLOTS / "bar-p999.svg", fmt_ns,
              "Open-loop 200k orders/s, 1.01M orders, client-send to engine-complete.")

    # ---- machine-readable --------------------------------------------------
    out = {
        "groups": summaries,
        "checksums": {k: [list(t) for t in v] for k, v in checksum_groups.items()},
        "flags": [{"config": c, "run": r, "p999": v, "median": m}
                  for c, r, v, m in flags],
    }
    (RESULTS / "analysis.json").write_text(json.dumps(out, indent=2) + "\n")

    print(f"wrote {RESULTS / 'summary.csv'}")
    print(f"wrote {RESULTS / 'summary.md'}")
    print(f"wrote {RESULTS / 'analysis.json'}")
    print(f"wrote {len(list(PLOTS.glob('*.svg')))} plot(s) to {PLOTS}")
    if flags:
        print(f"warning: {len(flags)} run(s) flagged as p99.9 outliers", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
