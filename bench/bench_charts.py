#!/usr/bin/env python3
# Turn vfs_bench results (--csv output, or Markdown tables from --md / the console) into SVG bar charts.
# Standard library only; matplotlib is not required.
#
# Usage:
#   python3 bench/bench_charts.py --platform linux --input linux.csv --out docs/images/bench
#
# Output:
#   <platform>-<scenario>.svg     one chart per scenario: two panels (1 thread, N threads, default 8),
#                                 each with its own scale, bars sorted fastest first, labeled with ops/s and speedup vs base:stdio
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
BASELINES = ("base:stdio", "base:fstream", "base:posix", "base:win32")
BASE_COLOR = "#a0a0a0"
VFS_COLOR = "#4e79a7"


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


def panel(out, x0, y0, width, caption, items, ref):
    """One panel: items = [(target, value)] sorted fastest-first; ref = base:stdio value (or None)."""
    bar_h, gap, label_w, value_w = 20, 6, 150, 120
    chart_w = width - label_w - value_w
    vmax = max(v for _, v in items)
    out.append('<text x="%d" y="%d" font-size="14" font-weight="bold">%s</text>' % (x0 + label_w, y0, html.escape(caption)))
    top = y0 + 12
    for i, (t, v) in enumerate(items):
        y = top + i * (bar_h + gap)
        base = t in BASELINES
        out.append('<text x="%d" y="%d" text-anchor="end"%s>%s</text>' % (
            x0 + label_w - 8, y + bar_h - 6, ' fill="#666"' if base else "", html.escape(t)))
        w = max(1.0, chart_w * v / vmax)
        out.append('<rect x="%d" y="%d" width="%.1f" height="%d" fill="%s"/>' % (x0 + label_w, y, w, bar_h, BASE_COLOR if base else VFS_COLOR))
        text = fmt(v) + ("  (%.1fx)" % (v / ref) if ref else "")
        out.append('<text x="%.1f" y="%d"%s>%s</text>' % (x0 + label_w + w + 5, y + bar_h - 6, ' fill="#666"' if base else "", html.escape(text)))
    bottom = top + len(items) * (bar_h + gap)
    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#888"/>' % (x0 + label_w, top - 2, x0 + label_w, bottom))
    if ref:
        rx = x0 + label_w + chart_w * ref / vmax
        out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="#333" stroke-dasharray="4,3"/>' % (rx, top - 4, rx, bottom))
        out.append('<text x="%.1f" y="%d" font-size="11" fill="#333" text-anchor="middle">stdio</text>' % (rx, bottom + 13))
    return bottom + 16


def bar_chart(title, subtitle, panels, out_path):
    """panels: [(caption, {target: value})]. Side-by-side horizontal bar panels, each with its own scale."""
    panel_w, top = 560, 78
    shown = [(c, d) for c, d in panels if d]
    width = max(1, len(shown)) * panel_w + 20
    out = []
    bottom = top
    for i, (caption, d) in enumerate(shown):
        items = sorted(d.items(), key=lambda kv: -kv[1])
        bottom = max(bottom, panel(out, 10 + i * panel_w, top, panel_w - 10, caption, items, d.get("base:stdio")))
    height = bottom + 10
    head = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="Segoe UI,Helvetica,Arial,sans-serif" font-size="13">' % (width, height),
            '<rect width="100%" height="100%" fill="white"/>',
            '<text x="10" y="24" font-size="17" font-weight="bold">%s</text>' % html.escape(title),
            '<text x="10" y="44" fill="#555">%s</text>' % html.escape(subtitle),
            '<text x="10" y="62" font-size="12" fill="#555">Bars sorted fastest first; label = ops/s (speedup vs base:stdio at the same thread count). Grey = direct-API baselines.</text>']
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(head + out + ["</svg>"]) + "\n")


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
        panels = [(cap, {t: after[(sc, t, th)] for t in targets if after.get((sc, t, th))})
                  for cap, th in (("1 thread", 1), ("%d threads" % n, n))]
        bar_chart("%s — %s (ops/s, higher is better)" % (args.platform, sc), args.label, panels,
                  os.path.join(args.out, "%s-%s.svg" % (args.platform, sc)))


if __name__ == "__main__":
    main()
