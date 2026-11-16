#!/usr/bin/env python3
"""Analyse isolated workload runs and emit Markdown, JSON, CSV, and SVG charts."""
from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path

NUMERIC = {"p50", "p95", "p99", "p999", "throughput", "accepted", "rejected",
           "invalid", "trades", "offered_rate", "queue_full_waits", "queue_empty_waits"}


def load(root: Path, recursive: bool = False) -> list[dict]:
    rows = []
    pattern = "*/*/run-*/runs.csv" if recursive else "run-*/runs.csv"
    for path in sorted(root.glob(pattern)):
        manifest_path = path.parent / "manifest.json"
        manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
        with path.open(newline="") as fh:
            for row in csv.DictReader(fh):
                for key in NUMERIC:
                    try:
                        row[key] = float(row[key])
                    except (KeyError, ValueError):
                        row[key] = 0.0
                row.update({"profile": manifest.get("profile", root.parent.name),
                            "invalid_generated": sum(manifest.get(k, 0) for k in
                                ("invalid_id", "invalid_side", "invalid_quantity")),
                            "duplicate_generated": manifest.get("duplicate_generated", 0),
                            "valid_generated": manifest.get("valid_generated", 0)})
                rows.append(row)
    return rows


def med(rows: list[dict], key: str) -> float:
    return float(statistics.median([r[key] for r in rows])) if rows else 0.0


def svg_bar(path: Path, title: str, rows: list[dict], key: str, color: str) -> None:
    width, height, left, top, bottom = 920, 420, 90, 60, 70
    chart_h = height - top - bottom
    maximum = max([r[key] for r in rows] + [1.0])
    group_width = (width - left - 45) / max(len(rows), 1)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
           '<style>text{font-family:Arial,sans-serif}.title{font-size:20px;font-weight:bold}'
           '.axis{stroke:#555}.grid{stroke:#dbe5ed}.label{font-size:12px}</style>',
           f'<text x="{width/2}" y="28" text-anchor="middle" class="title">{title}</text>']
    for i in range(5):
        y = top + chart_h - i * chart_h / 4
        v = maximum * i / 4
        out += [f'<line x1="{left}" y1="{y:.1f}" x2="{width-35}" y2="{y:.1f}" class="grid"/>',
                f'<text x="{left-8}" y="{y+4:.1f}" text-anchor="end" class="label">{v:.0f}</text>']
    out += [f'<line x1="{left}" y1="{top}" x2="{left}" y2="{height-bottom}" class="axis"/>',
            f'<line x1="{left}" y1="{height-bottom}" x2="{width-35}" y2="{height-bottom}" class="axis"/>']
    for i, row in enumerate(rows):
        x = left + i * group_width + group_width * 0.25
        bar_width = group_width * 0.5
        bar_height = row[key] / maximum * chart_h
        out.append(f'<rect x="{x:.1f}" y="{top+chart_h-bar_height:.1f}" width="{bar_width:.1f}" '
                   f'height="{bar_height:.1f}" fill="{color}" rx="4"/>')
        out.append(f'<text x="{x+bar_width/2:.1f}" y="{top+chart_h-bar_height-6:.1f}" '
                   f'text-anchor="middle" class="label">{row[key]:.1f}</text>')
        out.append(f'<text x="{x+bar_width/2:.1f}" y="{height-bottom+22}" '
                   f'text-anchor="middle" class="label">{row["profile"]}</text>')
    out.append('</svg>')
    path.write_text("\n".join(out))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", type=Path,
                        help="report directory; defaults to --input")
    parser.add_argument("--recursive", action="store_true",
                        help="compare profile/timestamp directories below --input")
    parser.add_argument("--offered-rate", type=float,
                        help="keep only server rows with this offered rate")
    args = parser.parse_args()
    root = args.input
    rows = load(root, args.recursive)
    if args.offered_rate is not None:
        rows = [r for r in rows if r["offered_rate"] == args.offered_rate]
    if not rows:
        raise SystemExit(f"no run CSVs under {root}")
    output = args.output or root
    output.mkdir(parents=True, exist_ok=True)
    summary = []
    groups = sorted({(r["profile"], r.get("config", "")) for r in rows})
    for profile, config in groups:
        group = [r for r in rows if r["profile"] == profile and r.get("config", "") == config]
        label = profile
        if len({r.get("config", "") for r in rows if r["profile"] == profile}) > 1:
            suffix = config.rsplit("-", 1)[-1] if config else "unknown"
            label = f"{profile} / {suffix}"
        summary.append({"profile": label, "runs": len(group),
                        **{key: med(group, key) for key in
                           ("p50", "p95", "p99", "p999", "throughput", "accepted",
                            "rejected", "invalid", "trades", "queue_full_waits",
                            "queue_empty_waits", "invalid_generated", "duplicate_generated",
                            "valid_generated")}})
    plots = output / "plots"
    plots.mkdir(exist_ok=True)
    svg_bar(plots / "latency-p50.svg", "Median latency by workload (us)", summary, "p50", "#1f4e79")
    svg_bar(plots / "latency-p999.svg", "p99.9 latency by workload (us)", summary, "p999", "#a2261e")
    svg_bar(plots / "throughput.svg", "Achieved throughput by workload (orders/s)", summary, "throughput", "#0f6e6e")
    with (output / "summary.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=summary[0].keys())
        writer.writeheader(); writer.writerows(summary)
    (output / "analysis.json").write_text(json.dumps({"runs": rows, "summary": summary}, indent=2))
    with (output / "summary.md").open("w") as fh:
        fh.write(f"# Workload results: {output.name}\n\n")
        fh.write(f"Runs: {len(rows)}. Baseline `results/raw/` was not read or modified.\n\n")
        fh.write("| profile | runs | p50 (us) | p95 (us) | p99 (us) | p99.9 (us) | throughput | accepted | rejected | parser invalid | generated invalid | duplicates | trades |\n")
        fh.write("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for r in summary:
            fh.write("| {profile} | {runs} | {p50:.1f} | {p95:.1f} | {p99:.1f} | {p999:.1f} | {throughput:.0f} | {accepted:.0f} | {rejected:.0f} | {invalid:.0f} | {invalid_generated:.0f} | {duplicate_generated:.0f} | {trades:.0f} |\n".format(**r))
        fh.write("\n## Charts\n\n![Median latency](plots/latency-p50.svg)\n\n![Tail latency](plots/latency-p999.svg)\n\n![Throughput](plots/throughput.svg)\n\n")
        fh.write("`invalid` is the server parser count. `generated invalid` and `duplicates` "
                 "come from the workload manifest; invalid wire records do not enter the book.\n")
    print(output / "summary.md")


if __name__ == "__main__":
    main()
