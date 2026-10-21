#!/usr/bin/env python3
# Turn vfs_bench results (--csv output, or Markdown tables from --md / the console) into SVG bar charts.
# Standard library only; matplotlib is not required.
#
# Usage:
#   python3 bench/bench_charts.py --platform linux --input linux.csv --out docs/images/bench
#
# Output:
#   <platform>-<scenario>.svg     one chart per scenario: throughput (ops/s) of each target at 1 thread and N threads (default 8)
import argparse
import csv
import html
import os
import re
import sys

DEFAULT_SCENARIOS = ["read.small", "read.large.rand", "meta.open", "meta.stat", "meta.miss", "write.churn"]
# Targets shown in the bar chart (order is display order); missing ones are skipped.
DEFAULT_TARGETS = ["base:stdio", "base:posix", "base:win32", "vfs:native", "vfs:memory", "vfs:archive",
                   "vfs:archive-lz4", "vfs:archive+native", "vfs:memory+archive", "vfs:3layer"]
COLORS = ["#4e79a7", "#f28e2b", "#59a14f", "#e15759"]


def parse_number(text):
    """Parse a value of the form "1.23M" / "456.7k" / "789"."""
    text = text.strip()
    m = re.fullmatch(r"([0-9.]+)\s*([kKMG]?)", text)
    if not m:
        return None
    mult = {"": 1, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]
    return float(m.group(1)) * mult


def load(path):
    """Return {(scenario, target, threads): ops_per_sec}."""
    data = {}
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if text.startswith("scenario,"):
        for row in csv.DictReader(text.splitlines()):
            if row.get("failed", "0") not in ("0", ""):
                continue
            data[(row["scenario"], row["target"], int(row["threads"]))] = float(row["ops_per_sec"])
        return data
    # Markdown: after ### <scenario> comes a table | target | threads | ops/s | ...
    scenario = None
    for line in text.splitlines():
        h = re.match(r"^#{2,4}\s+([a-z]+\.[a-z.]+)\s*$", line)
        if h:
            scenario = h.group(1)
            continue
        if scenario and line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) < 3 or not cells[1].isdigit():
                continue
            v = parse_number(cells[2])
            if v is not None:
                data[(scenario, cells[0], int(cells[1]))] = v
    return data


def fmt(v):
    if v >= 1e6:
        return "%.2fM" % (v / 1e6)
    if v >= 1e3:
        return "%.1fk" % (v / 1e3)
    return "%.0f" % v


def bar_chart(title, subtitle, labels, series, series_names, out_path):
    """Horizontal grouped bar chart. series: [[value or None, ...], ...], one color per series."""
    bar_h, gap, label_w, chart_w, right_pad = 14, 10, 170, 520, 70
    group_h = bar_h * len(series) + gap
    top = 64
    height = top + group_h * len(labels) + 30
    width = label_w + chart_w + right_pad
    vmax = max([v for s in series for v in s if v] or [1])
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="Segoe UI,Helvetica,Arial,sans-serif" font-size="12">' % (width, height),
           '<rect width="100%" height="100%" fill="white"/>',
           '<text x="10" y="22" font-size="15" font-weight="bold">%s</text>' % html.escape(title),
           '<text x="10" y="40" fill="#555">%s</text>' % html.escape(subtitle)]
    # Legend.
    x = label_w
    for i, name in enumerate(series_names):
        out.append('<rect x="%d" y="48" width="10" height="10" fill="%s"/>' % (x, COLORS[i % len(COLORS)]))
        out.append('<text x="%d" y="57">%s</text>' % (x + 14, html.escape(name)))
        x += 24 + 7 * len(name)
    for gi, label in enumerate(labels):
        y0 = top + gi * group_h
        out.append('<text x="%d" y="%d" text-anchor="end">%s</text>' % (label_w - 8, y0 + bar_h * len(series) / 2 + 4, html.escape(label)))
        for si, s in enumerate(series):
            v = s[gi]
            y = y0 + si * bar_h
            if not v:
                out.append('<text x="%d" y="%d" fill="#999">n/a</text>' % (label_w + 4, y + bar_h - 3))
                continue
            w = max(1, chart_w * v / vmax)
            out.append('<rect x="%d" y="%d" width="%.1f" height="%d" fill="%s"/>' % (label_w, y, w, bar_h - 2, COLORS[si % len(COLORS)]))
            out.append('<text x="%.1f" y="%d">%s</text>' % (label_w + w + 4, y + bar_h - 3, fmt(v)))
    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#888"/>' % (label_w, top - 4, label_w, height - 26))
    out.append('</svg>')
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description="Convert vfs_bench results to SVG bar charts")
    ap.add_argument("--platform", required=True, help="platform name used in titles and file names, e.g. linux")
    ap.add_argument("--input", required=True, help="vfs_bench --csv output or Markdown tables")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--threads", type=int, default=8, help="multi-thread column to compare against 1 thread (default 8)")
    ap.add_argument("--label", default="", help="extra subtitle text, e.g. CPU model")
    args = ap.parse_args()

    after = load(args.input)
    if not after:
        sys.exit("no results found in " + args.input)
    os.makedirs(args.out, exist_ok=True)
    n = args.threads
    for sc in DEFAULT_SCENARIOS:
        targets = [t for t in DEFAULT_TARGETS if (sc, t, 1) in after or (sc, t, n) in after]
        if not targets:
            continue
        bar_chart("%s — %s (ops/s, higher is better)" % (args.platform, sc), args.label, targets,
                  [[after.get((sc, t, 1)) for t in targets], [after.get((sc, t, n)) for t in targets]],
                  ["1 thread", "%d threads" % n], os.path.join(args.out, "%s-%s.svg" % (args.platform, sc)))


if __name__ == "__main__":
    main()
