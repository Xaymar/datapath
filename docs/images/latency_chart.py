#!/usr/bin/env python3
"""Draws docs/images/latency-p999.svg from the README's p99.9 tables.

Edit DATA (and FOOTNOTES) below after a new benchmark run, then run
    python3 docs/images/latency_chart.py
Values are p99.9 round-trip latency in microseconds.
"""
import math
import os

SIZES = ["16 B", "64 B", "256 B", "1 KiB", "4 KiB", "16 KiB", "64 KiB", "256 KiB", "1 MiB"]

# panel title -> one entry per size: ((median, lo, hi) for DataPath,
# libuv as shipped, libuv + policy). median = median p99.9 of the runs,
# lo/hi = lowest/highest p99.9 of any run (the benchmark's "p99.9 lo/hi").
def _rows(*series):
    # each series is (medians, lows, highs), one value per size
    return [tuple(tuple(s[k][i] for k in range(3)) for s in series) for i in range(len(series[0][0]))]


DATA = {
    "Windows · Irkela · idle, --pin 8,12": _rows(
        ([10.9, 7.1, 7.3, 7.5, 12.5, 24.3, 29.6, 78.6, 794.2],
         [7.0, 6.8, 7.0, 6.9, 11.4, 14.8, 28.5, 74.8, 792.6],
         [13.0, 7.2, 7.6, 7.9, 14.2, 25.7, 39.1, 79.5, 819.5]),
        ([29.5, 29.6, 29.0, 30.4, 33.8, 43.7, 56.8, 250.3, 1177.8],
         [29.0, 29.5, 28.7, 28.8, 33.1, 42.7, 55.4, 176.7, 1143.7],
         [30.2, 30.1, 29.1, 34.2, 34.0, 44.2, 63.7, 284.3, 1183.4]),
        ([15.2, 16.2, 14.8, 17.0, 19.8, 31.3, 51.6, 221.3, 1197.9],
         [14.5, 14.5, 14.7, 14.7, 19.3, 30.2, 51.0, 205.3, 1143.7],
         [17.1, 17.3, 15.3, 19.8, 20.1, 32.6, 54.1, 287.4, 1887.3]),
    ),
    "Windows · Irkela · --stress 0 --pin 8,12": _rows(
        ([39.0, 39.2, 42.2, 41.8, 102.4, 228.9, 457.3, 2178.0, 28187.9],
         [38.1, 38.0, 38.0, 39.2, 99.3, 227.2, 441.9, 1851.9, 26616.2],
         [45.2, 39.7, 43.7, 111.6, 109.1, 231.2, 463.8, 3242.3, 29051.2]),
        ([256.6, 288.8, 255.6, 264.7, 334.2, 692.3, 2981.6, 36566.1, 73953.4],
         [245.8, 274.6, 232.6, 249.6, 330.0, 474.6, 1644.1, 4690.0, 61908.2],
         [280.8, 304.8, 266.6, 273.2, 370.3, 1008.7, 3657.0, 43400.0, 77841.2]),
        ([107.5, 121.5, 111.6, 117.3, 198.4, 313.1, 639.6, 7465.4, 31855.6],
         [103.2, 110.6, 96.8, 106.9, 184.7, 292.3, 618.2, 2915.0, 30593.0],
         [115.1, 130.1, 117.7, 164.2, 208.0, 351.4, 721.2, 11543.8, 33049.7]),
    ),
    "Linux · merenka-gpu0 · idle": _rows(
        ([13.73, 13.10, 13.82, 15.07, 21.81, 35.84, 79.28, 133.18, 567.44],
         [12.59, 12.24, 13.41, 14.64, 20.60, 33.79, 66.92, 122.30, 501.09],
         [14.56, 14.50, 14.24, 15.54, 23.84, 40.32, 86.21, 140.53, 842.52]),
        ([17.10, 17.36, 17.77, 17.80, 23.10, 30.52, 55.27, 106.70, 500.82],
         [15.81, 16.23, 16.96, 17.36, 22.26, 30.01, 53.03, 99.88, 290.41],
         [17.85, 19.08, 18.50, 18.23, 28.09, 31.68, 62.32, 118.74, 673.17]),
        ([13.95, 13.82, 14.19, 15.10, 20.24, 31.61, 65.89, 192.93, 766.14],
         [13.46, 13.26, 13.99, 14.53, 19.24, 27.42, 64.47, 128.15, 714.41],
         [14.34, 14.35, 14.65, 16.06, 20.47, 32.14, 74.29, 221.53, 936.21]),
    ),
    "Linux · merenka-gpu0 · --stress 0 --stress-nice 12": _rows(
        ([11.94, 12.33, 20.22, 13.41, 19.76, 45.64, 482.82, 4835.55, 8143.60],
         [11.80, 12.17, 11.96, 13.15, 19.20, 40.39, 122.58, 341.94, 951.65],
         [12.57, 205.60, 199.12, 13.66, 205.09, 272.09, 566.58, 8271.23, 10184.46]),
        ([163.63, 78.89, 58.48, 61.60, 238.10, 360.33, 4150.95, 6847.36, 10959.15],
         [84.60, 69.09, 48.32, 55.08, 131.40, 296.20, 4136.74, 4887.34, 10576.70],
         [186.06, 179.60, 74.61, 85.53, 286.25, 985.93, 4175.46, 8555.97, 13484.26]),
        ([42.74, 150.18, 185.40, 186.59, 25.04, 219.19, 559.57, 358.50, 12391.71],
         [37.22, 14.96, 150.86, 16.31, 20.02, 32.89, 120.07, 344.53, 10468.11],
         [188.26, 212.99, 189.82, 210.86, 228.24, 243.16, 4155.47, 752.06, 15892.21]),
    ),
}

FOOTNOTES = [
    "Ping-pong between two processes, release builds. Mark: median p99.9 of 5 runs; whisker: lowest to highest p99.9 of any run.",
    "2026-11-22. Irkela (commit cf179fe; client pinned to cpu8, server to cpu12): Ryzen 9 7940HS 8C/16T, Windows 10 22H2, Balanced.",
    "merenka-gpu0 (commit fef98d8, unpinned): EPYC 7313P container limited to 8C/16T, Linux 6.12, GCC 15.2. Tables in README.",
]

SERIES = [("dp", "DataPath"), ("uv", "libuv as shipped"), ("up", "libuv + DataPath's spin/priority")]

W, X0, X1 = 760, 80.0, 728.0
ROW, HEAD = 26, 27


def ticks(lo, hi):
    out, d = [], 10 ** math.floor(math.log10(lo))
    while True:
        for m in (1, 2, 5):
            v = m * d
            if v > hi * 1.0001:
                return out
            out.append(v)
        d *= 10


def axis(values):
    lo = min(values)
    hi = max(values)
    t = ticks(1, 10 ** 9)
    lo_t = max(v for v in t if v <= lo)
    hi_t = min(v for v in t if v >= hi)
    return lo_t, hi_t, [v for v in t if lo_t <= v <= hi_t]


def fmt(v):
    return f"{v / 1000:g}k" if v >= 1000 else f"{v:g}"


def marker(kind, x, y, title):
    t = f"<title>{title}</title>"
    if kind == "dp":
        return f'<circle class="dp m" cx="{x:.1f}" cy="{y - 4:.1f}" r="5">{t}</circle>'
    if kind == "uv":
        return f'<rect class="uv m" x="{x - 4.5:.1f}" y="{y - 4.5:.1f}" width="9" height="9" rx="1.5">{t}</rect>'
    return f'<path class="up m" d="M{x:.1f},{y - 2:.1f} l6,6 l-6,6 l-6,-6 z">{t}</path>'


def main():
    allv = [v for rows in DATA.values() for r in rows for s in r for v in s]
    lo, hi, tk = axis(allv)
    scale = (X1 - X0) / math.log10(hi / lo)

    def X(v):
        return X0 + math.log10(v / lo) * scale

    block = 10 + len(SIZES) * ROW + 13  # panel title to its axis labels
    last_axis = 78 + (len(DATA) - 1) * (block + HEAD) + block
    height = last_axis + 23 + 15 * (len(FOOTNOTES) - 1) + 13
    o = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {height}" width="{W}" height="{height}" role="img" '
        'aria-label="Round-trip p99.9 latency, DataPath versus libuv, on Windows and Linux, idle and under stress, 16 B to 1 MiB"><style>\n'
        "svg{--bg:#fcfcfb;--t1:#0b0b0b;--t2:#52514e;--grid:#e6e5e1;--dp:#2a78d6;--uv:#eb6834;--up:#1baf7a}\n"
        "@media (prefers-color-scheme: dark){svg{--bg:#1a1a19;--t1:#f0efec;--t2:#c3c2b7;--grid:#34332f;--dp:#3987e5;--uv:#d95926;--up:#199e70}}\n"
        '.bg{fill:var(--bg)} text{font-family:-apple-system,"Segoe UI",Helvetica,Arial,sans-serif}\n'
        ".t1{font-size:16px;font-weight:600;fill:var(--t1)} .t2{font-size:13px;font-weight:600;fill:var(--t1)}\n"
        ".t3{font-size:12px;fill:var(--t2)} .t4{font-size:11px;fill:var(--t2);font-variant-numeric:tabular-nums}\n"
        ".grid{stroke:var(--grid);stroke-width:1} .wk{stroke-width:1.5;stroke-linecap:round;opacity:.55}\n"
        ".m{stroke:var(--bg);stroke-width:2} .dp{fill:var(--dp)} .uv{fill:var(--uv)} .up{fill:var(--up)}\n"
        ".wk.dp{stroke:var(--dp)} .wk.uv{stroke:var(--uv)} .wk.up{stroke:var(--up)}\n"
        f'</style><rect class="bg" width="{W}" height="{height}" rx="6"/>',
        '<text class="t1" x="16" y="28">Round-trip latency, p99.9 (µs, log scale, lower is better)</text>',
        '<circle class="dp m" cx="21.0" cy="47.0" r="5"></circle><text class="t3" x="31" y="51">DataPath</text>',
        '<rect class="uv m" x="106.3" y="42.5" width="9" height="9" rx="1.5"></rect><text class="t3" x="120.8" y="51">libuv as shipped</text>',
        '<path class="up m" d="M253.4,41.0 l6,6 l-6,6 l-6,-6 z"></path><text class="t3" x="263.4" y="51">libuv + DataPath\'s spin/priority</text>',
    ]
    y = 78
    for name, rows in DATA.items():
        assert len(rows) == len(SIZES), name
        o.append(f'<text class="t2" x="16" y="{y}">{name}</text>')
        g0, g1 = y + 10, y + 10 + len(SIZES) * ROW
        for v in tk:
            o.append(f'<line class="grid" x1="{X(v):.1f}" x2="{X(v):.1f}" y1="{g0}" y2="{g1}"/>')
        for i, (size, vals) in enumerate(zip(SIZES, rows)):
            ry = y + 27 + i * ROW
            my = ry - 4
            o.append(f'<text class="t3" x="70" y="{ry}.0" text-anchor="end">{size}</text>')
            # libuv first so DataPath sits on top; marks are dodged vertically
            for k in (1, 2, 0):
                kind, label = SERIES[k]
                med, lo_v, hi_v = vals[k]
                wy = my + (-4, 0, 4)[k]
                o.append(f'<line class="wk {kind}" x1="{X(lo_v):.1f}" x2="{X(hi_v):.1f}" y1="{wy}" y2="{wy}"/>')
            for k in (1, 2, 0):
                kind, label = SERIES[k]
                med, lo_v, hi_v = vals[k]
                o.append(marker(kind, X(med), my, f"{name}, {size}, {label}: {med:g} µs (runs {lo_v:g} to {hi_v:g})"))
        ay = g1 + 13
        for v in tk:
            o.append(f'<text class="t4" x="{X(v):.1f}" y="{ay}" text-anchor="middle">{fmt(v)}</text>')
        y = ay + HEAD
    y = y - HEAD + 23
    for line in FOOTNOTES:
        o.append(f'<text class="t4" x="16" y="{y}">{line}</text>')
        y += 15
    o.append("</svg>\n")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "latency-p999.svg")
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(o))


if __name__ == "__main__":
    main()
