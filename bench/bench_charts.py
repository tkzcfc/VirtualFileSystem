#!/usr/bin/env python3
# Turn vfs_bench results (--csv output, or Markdown tables from --md / the console) into SVG bar charts.
# Standard library only; matplotlib is not required.
#
# Usage:
#   python3 bench/bench_charts.py --platform linux --input linux.csv --out docs/images/bench
#
# Output:
#   <platform>.svg   one heatmap per platform: two tables side by side (1 thread | N threads, default 8);
#                    rows = targets, columns = scenarios, cell = speedup vs base:stdio (log-scale diverging color)
#                    with the absolute ops/s below it
import argparse
import csv
import html
import os
import re
import sys

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


import math

# Columns (scenarios) in display order, with short headers.
SCENARIOS = [("read.small", "small read"), ("read.medium", "medium read"), ("read.large.seq", "large seq"),
             ("read.large.rand", "large rand"), ("meta.open", "open"), ("meta.stat", "stat"),
             ("meta.miss", "miss"), ("meta.list", "list"), ("write.create", "create"), ("write.churn", "churn"),
             ("write.large.seq", "large write"), ("write.overwrite", "overwrite"), ("write.append", "append")]
# Rows (targets) in display order; missing ones are skipped.
ROWS = ["base:stdio", "base:posix", "base:win32", "vfs:native", "vfs:memory", "vfs:archive", "vfs:archive-lz4",
        "vfs:archive+native", "vfs:memory+archive", "vfs:3layer"]
SLOW = (196, 64, 52)    # < 1x (slower than stdio)
FAST = (27, 120, 80)    # > 1x (faster than stdio)
CAP = 2.0               # color saturates at 100x / 0.01x (log10 = 2)


def cell_color(r):
    t = max(-1.0, min(1.0, math.log10(r) / CAP))
    end = FAST if t > 0 else SLOW
    a = abs(t) ** 0.8
    rgb = tuple(round(255 + (c - 255) * a) for c in end)
    return "#%02x%02x%02x" % rgb, a > 0.55


def heatmap(out, x0, y0, caption, data, threads, rows, cols, show_rows):
    label_w = 150 if show_rows else 0
    cw, ch, hh = 74, 40, 26
    out.append('<text x="%.1f" y="%d" font-size="15" font-weight="bold" text-anchor="middle">%s</text>' % (x0 + label_w + cw * len(cols) / 2, y0, html.escape(caption)))
    y = y0 + 10
    for j, (_, lab) in enumerate(cols):
        out.append('<text x="%.1f" y="%d" text-anchor="middle" font-size="12" fill="#333">%s</text>' % (x0 + label_w + cw * (j + 0.5), y + 16, html.escape(lab)))
    y += hh
    for i, t in enumerate(rows):
        yy = y + i * ch
        if show_rows:
            out.append('<text x="%d" y="%.1f" text-anchor="end"%s>%s</text>' % (x0 + label_w - 8, yy + ch / 2 + 5, ' fill="#555"' if t.startswith("base:") else "", html.escape(t)))
        for j, (sc, _) in enumerate(cols):
            x = x0 + label_w + j * cw
            v, ref = data.get((sc, t, threads)), data.get((sc, "base:stdio", threads))
            if not v or not ref:
                out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="#f4f4f4" stroke="#fff"/>' % (x, yy, cw, ch))
                out.append('<text x="%.1f" y="%.1f" text-anchor="middle" fill="#bbb">–</text>' % (x + cw / 2, yy + ch / 2 + 5))
                continue
            r = v / ref
            color, dark = cell_color(r)
            out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="%s" stroke="#fff"/>' % (x, yy, cw, ch, color))
            txt = ("%.0fx" % r) if r >= 99.5 else ("%.1fx" % r) if r >= 0.095 else ("%.2fx" % r)
            out.append('<text x="%.1f" y="%.1f" text-anchor="middle" font-weight="bold" fill="%s">%s</text>' % (x + cw / 2, yy + 18, "#fff" if dark else "#111", txt))
            out.append('<text x="%.1f" y="%.1f" text-anchor="middle" font-size="10.5" fill="%s">%s</text>' % (x + cw / 2, yy + 33, "#e8e8e8" if dark else "#666", fmt(v)))
    return x0 + label_w + cw * len(cols), y + len(rows) * ch


def render(title, subtitle, data, threads, out_path):
    present = {(sc, t) for (sc, t, _) in data}
    rows = [t for t in ROWS if any((sc, t) in present for sc, _ in SCENARIOS)]
    cols = [(sc, lab) for sc, lab in SCENARIOS if (sc, "base:stdio") in present]
    out = []
    top = 92
    x_end, bottom = heatmap(out, 10, top, "1 thread", data, 1, rows, cols, True)
    x_end, bottom = heatmap(out, x_end + 30, top, "%d threads" % threads, data, threads, rows, cols, False)
    width = x_end + 20
    # Color bar legend.
    ly = bottom + 24
    stops = [0.01, 0.1, 0.5, 1, 2, 10, 100]
    bw = 60
    out.append('<text x="10" y="%d" font-size="12" fill="#333">Speedup vs base:stdio (log color scale):</text>' % (ly + 14))
    for k, v in enumerate(stops):
        c, dark = cell_color(v)
        x = 260 + k * bw
        out.append('<rect x="%d" y="%d" width="%d" height="20" fill="%s" stroke="#ddd"/>' % (x, ly, bw, c))
        out.append('<text x="%.1f" y="%d" text-anchor="middle" font-size="11" fill="%s">%gx</text>' % (x + bw / 2, ly + 14, "#fff" if dark else "#111", v))
    out.append('<text x="10" y="%d" font-size="11" fill="#666">Small grey number = absolute throughput in ops/s. "–" = target not run for that scenario (archive backends are read-only).</text>' % (ly + 42))
    height = ly + 56
    head = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="Segoe UI,Helvetica,Arial,sans-serif" font-size="13">' % (width, height),
            '<rect width="100%" height="100%" fill="white"/>',
            '<text x="10" y="26" font-size="18" font-weight="bold">%s</text>' % html.escape(title),
            '<text x="10" y="48" fill="#555">%s</text>' % html.escape(subtitle),
            '<text x="10" y="68" font-size="12" fill="#555">Cell = throughput / base:stdio throughput for the same scenario and thread count; green is faster, red is slower.</text>']
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(head + out + ["</svg>"]) + "\n")


def main():
    ap = argparse.ArgumentParser(description="Convert vfs_bench results to one SVG heatmap per platform")
    ap.add_argument("--platform", required=True, help="platform name used in the title and file name, e.g. linux")
    ap.add_argument("--input", required=True, help="vfs_bench --csv output or Markdown tables")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--threads", type=int, default=8, help="multi-thread table shown next to 1 thread (default 8)")
    ap.add_argument("--label", default="", help="subtitle text, e.g. CPU, OS, compiler")
    args = ap.parse_args()

    data = load(args.input)
    if not data:
        sys.exit("no results found in " + args.input)
    os.makedirs(args.out, exist_ok=True)
    render("%s — speed relative to stdio (higher is better)" % args.platform, args.label, data, args.threads,
           os.path.join(args.out, "%s.svg" % args.platform))


if __name__ == "__main__":
    main()
