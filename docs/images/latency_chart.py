#!/usr/bin/env python3
"""Draws one p99.9 graph per benchmark machine from the README's p99.9 tables:
docs/images/latency-p999-windows.svg and docs/images/latency-p999-linux.svg.

Edit CHARTS below after a new benchmark run, then run
    python3 docs/images/latency_chart.py
Values are p99.9 round-trip latency in microseconds.
"""
import math
import os

SIZES = ["16 B", "64 B", "256 B", "1 KiB", "4 KiB", "16 KiB", "64 KiB", "256 KiB", "1 MiB"]

# panel title -> one entry per size: one (median, lo, hi) per series, in the
# order of the chart's "series" list (keys of SERIES below); None where a series
# has no result for that size. median = median p99.9 of the runs,
# lo/hi = lowest/highest p99.9 of any run (the benchmark's "p99.9 lo/hi").
def _rows(*series):
    # each series is (medians, lows, highs), one value per size
    return [tuple(tuple(s[k][i] for k in range(3)) for s in series) for i in range(len(series[0][0]))]


def _missing():
    # a series that has no result at any size (did not finish)
    return ([None] * len(SIZES), [None] * len(SIZES), [None] * len(SIZES))


CHARTS = {
    "latency-p999-windows.svg": {
        "label": "Windows (Irkela)",
        "series": ["dp", "uv", "up", "ci", "bi"],
        "panels": {
            "idle, --pin 8,12": _rows(
                ([8.2, 2.2, 2.1, 2.6, 4.1, 8.6, 18.9, 42.3, 130.1],
                 [8.1, 1.9, 1.7, 2.5, 3.2, 7.4, 17.0, 40.8, 121.3],
                 [10.5, 8.1, 2.4, 2.9, 4.6, 9.0, 19.6, 48.1, 137.9]),
                ([31.0, 30.7, 29.8, 30.6, 34.5, 44.7, 65.8, 278.0, 1074.9],
                 [30.6, 30.2, 29.5, 29.9, 33.8, 44.5, 65.0, 219.9, 1058.2],
                 [32.3, 31.4, 30.7, 34.4, 35.3, 46.2, 72.3, 355.0, 1132.9]),
                ([15.7, 15.2, 15.1, 15.2, 22.2, 34.6, 73.5, 278.9, 9659.3],
                 [15.2, 14.9, 14.9, 14.7, 21.9, 33.2, 54.5, 271.6, 5033.4],
                 [16.1, 15.9, 15.8, 17.4, 23.3, 34.8, 76.4, 357.6, 15599.3]),
                # cpp-ipc and Boost.Interprocess as shipped: 2026-11-25, commit 31766bc
                ([16.8, 18.0, 28.2, 27.5, 30.6, 42.5, 152.5, 104.4, 569.1],
                 [9.6, 11.3, 26.4, 27.1, 28.7, 41.4, 68.4, 87.6, 446.3],
                 [18.3, 19.6, 32.3, 31.9, 33.2, 47.7, 190.8, 344.7, 771.1]),
                ([0.7, 1.1, 1.5, 1.1, 4.1, 28.9, 54.5, 207.9, 6310.1],
                 [0.6, 0.9, 1.1, 0.9, 3.6, 13.9, 19.8, 94.8, 1182.2],
                 [3.1, 3.2, 3.5, 1.5, 10.1, 33.2, 58.3, 43150.6, 44969.1]),
            ),
            # DataPath: three invocations of --runs 5; mark = median of the three
            # medians, whisker = lowest/highest p99.9 of all 15 runs.
            "--stress 0 --pin 8,12": _rows(
                ([5.1, 5.8, 7.2, 15.3, 25.6, 74.3, 527.6, 3946.4, 24394.1],
                 [4.4, 5.3, 6.3, 13.7, 24.8, 67.7, 417.4, 2219.8, 14600.1],
                 [7.7, 8.1, 10.9, 17.6, 29.1, 79.1, 548.2, 6522.5, 25999.3]),
                ([243.9, 265.3, 282.0, 292.9, 400.6, 1076.5, 1600.1, 36406.3, 90841.6],
                 [236.8, 247.9, 265.8, 263.8, 336.1, 967.2, 1512.0, 4459.4, 66229.2],
                 [298.6, 759.4, 348.6, 317.1, 519.9, 1545.8, 3273.6, 45182.4, 104482.7]),
                ([104.7, 103.8, 113.9, 110.9, 187.2, 273.2, 652.9, 7468.7, 31434.5],
                 [103.6, 97.4, 106.4, 109.8, 179.7, 247.7, 529.5, 4939.1, 30686.2],
                 [123.9, 107.1, 123.2, 159.7, 193.4, 1155.7, 670.0, 10480.6, 31721.5]),
                # cpp-ipc and Boost.Interprocess as shipped did not finish 16 B in 30 minutes
                _missing(), _missing(),
            ),
        },
        "footnotes": [
            "Ping-pong between two processes, release builds, 2026-11-23, commit df30376, DataPath with options::shared_memory.",
            "Mark: median p99.9 of 5 runs (DataPath under stress: median of 3 x 5 runs); whisker: lowest to highest p99.9 of any run.",
            "cpp-ipc 2e28547 and Boost.Interprocess 1.92 message_queue as shipped: 2026-11-25; neither finished 16 B under stress in 30 minutes.",
            "Irkela (client cpu8, server cpu12: different cores; one CCD): Ryzen 9 7940HS 8C/16T, Windows 10 22H2, Balanced.",
        ],
    },
    "latency-p999-linux.svg": {
        "label": "Linux (merenka-gpu0)",
        "series": ["dp", "uv", "up", "mj", "ci", "bi"],
        "panels": {
            "idle, --pin 1,5": _rows(
                ([1.18, 1.43, 1.43, 1.83, 3.44, 8.93, 21.98, 76.59, 321.33],
                 [1.15, 1.42, 1.40, 1.77, 3.17, 8.50, 20.07, 68.82, 308.69],
                 [1.22, 1.44, 1.60, 1.86, 3.90, 9.09, 22.35, 94.76, 328.37]),
                ([23.07, 22.72, 23.99, 24.40, 28.29, 37.43, 63.54, 147.06, 557.48],
                 [22.13, 22.48, 22.78, 23.92, 27.14, 36.54, 62.36, 141.18, 544.59],
                 [23.85, 23.49, 24.40, 25.37, 28.63, 39.03, 69.61, 153.37, 600.70]),
                ([13.68, 13.66, 14.27, 15.24, 19.62, 26.25, 56.31, 136.24, 541.41],
                 [13.55, 13.44, 13.89, 14.99, 18.80, 25.66, 56.11, 134.37, 518.54],
                 [13.84, 13.74, 14.41, 15.65, 21.05, 28.17, 57.61, 143.68, 577.22]),
                # Chromium Mojo bindings on the main thread: one invocation, no per-run spread
                ([60.5, 61.7, 62.5, 63.1, 72.8, 86.6, 439, 2003, 8704], [60.5, 61.7, 62.5, 63.1, 72.8, 86.6, 439, 2003, 8704], [60.5, 61.7, 62.5, 63.1, 72.8, 86.6, 439, 2003, 8704]),
                # cpp-ipc and Boost.Interprocess as shipped: 2026-11-25, commit a6a8085
                ([4.74, 4.74, 6.98, 7.16, 8.22, 12.48, 28.72, 168.59, 340.55],
                 [4.71, 4.71, 6.88, 6.93, 8.2, 12.34, 26.87, 92.76, 335.66],
                 [4.84, 4.87, 7.12, 7.21, 8.59, 12.81, 31.64, 172.54, 354.74]),
                ([31.84, 32.2, 31.37, 31.75, 40.88, 57.88, 128.32, 402.58, 1154.52],
                 [31.08, 31.3, 31.17, 31.48, 37.58, 54.26, 127.91, 396.11, 1138.85],
                 [40.86, 34.36, 34.94, 32.39, 55.43, 64.42, 128.79, 415.48, 1277.86]),
            ),
            # DataPath: three invocations of --runs 5; mark = median of the three
            # medians, whisker = lowest/highest p99.9 of all 15 runs.
            "--stress 0 --stress-nice 12 --pin 1,5": _rows(
                ([10.25, 10.11, 10.80, 13.15, 22.78, 50.76, 4058.28, 4207.28, 6395.99],
                 [7.09, 9.19, 9.40, 11.88, 20.36, 36.63, 470.38, 4176.52, 4585.79],
                 [10.98, 13.91, 11.38, 14.37, 44.74, 284.11, 4137.08, 5324.98, 7306.78]),
                ([4073.64, 4069.78, 4070.30, 4065.31, 4075.53, 4094.22, 4181.48, 7415.50, 12101.62],
                 [4068.21, 4066.91, 4061.47, 4062.51, 4070.61, 4089.45, 4173.06, 7159.14, 11730.21],
                 [4086.51, 4073.94, 4074.32, 4078.71, 4078.28, 4096.41, 4191.83, 7582.82, 12489.10]),
                ([3381.64, 3494.26, 3553.31, 3884.92, 4924.09, 4754.79, 4642.57, 7557.98, 12965.81],
                 [354.75, 2956.09, 626.38, 3266.77, 4569.23, 4711.27, 4381.94, 7285.19, 12568.06],
                 [4086.44, 3888.66, 3669.02, 3992.47, 5628.01, 5171.00, 4996.95, 7786.79, 14202.71]),
                ([7579, 7852, 7867, 7853, 7997, 8127, 10082, 19179, 53033], [7579, 7852, 7867, 7853, 7997, 8127, 10082, 19179, 53033], [7579, 7852, 7867, 7853, 7997, 8127, 10082, 19179, 53033]),
                # cpp-ipc as shipped: three invocations, none got past 4 KiB in 30 minutes
                ([7920.64, 5758.12, 7995.38, 7999.01, 8014.86, None, None, None, None],
                 [4040.38, 4074.47, 7960.06, 7966.73, 7995.62, None, None, None, None],
                 [7979.94, 7957.7, 8014.29, 8016.63, 8023.89, None, None, None, None]),
                # Boost.Interprocess as shipped: one invocation
                ([385.22, 464.04, 369.4, 370.3, 312.51, 3007.89, 4687.67, 10829.08, 18060.99],
                 [335.07, 256.82, 347.79, 324.94, 231.45, 825.91, 4523.17, 9928.26, 6689.31],
                 [1676.73, 753.6, 527.79, 728.18, 854.45, 4052.85, 5289.63, 11775.37, 18921.57]),
            ),
        },
        "footnotes": [
            "Ping-pong between two processes, release builds, 2026-11-23, commit cdc663a, DataPath with options::shared_memory.",
            "Mark: median p99.9 of 5 runs (DataPath under stress: median of 3 x 5 runs); whisker: lowest to highest p99.9 of any run.",
            "Chromium Mojo (a57585d): mojom bindings on the main thread, as browser code uses them; one invocation of 5 runs, so no whisker.",
            "cpp-ipc 2e28547 and Boost.Interprocess 1.92 message_queue as shipped: 2026-11-25; cpp-ipc under stress: median of 3 invocations,",
            "none of which got past 4 KiB in 30 minutes.",
            "merenka-gpu0 (client cpu1, server cpu5: different cores and CCDs): EPYC 7313P container 8C/16T, Linux 6.12, GCC 15.2.",
        ],
    },
}

SERIES = {
    "dp": "DataPath (shared memory)",
    "uv": "libuv as shipped",
    "up": "libuv + DataPath's spin/priority",
    "mj": "Chromium Mojo (bindings)",
    "ci": "cpp-ipc as shipped",
    "bi": "Boost.Interprocess as shipped",
}
NAMES = {"uv": "libuv", "mj": "Chromium Mojo", "ci": "cpp-ipc", "bi": "Boost.Interprocess"}
COLOURS = {  # light, dark
    "dp": ("#2a78d6", "#3987e5"),
    "uv": ("#eb6834", "#d95926"),
    "up": ("#1baf7a", "#199e70"),
    "mj": ("#8a5cc7", "#a37ae0"),
    "ci": ("#c93f8a", "#e0629f"),
    "bi": ("#9c7a12", "#c9a227"),
}
# drawn back to front, so DataPath ends up on top
DRAW_ORDER = ["mj", "bi", "ci", "uv", "up", "dp"]

W, X0, X1 = 760, 80.0, 728.0
HEAD = 27


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
    t = ticks(0.1, 10 ** 9)
    lo_t = max(v for v in t if v <= lo)
    hi_t = min(v for v in t if v >= hi)
    return lo_t, hi_t, [v for v in t if lo_t <= v <= hi_t]


def fmt(v):
    return f"{v / 1000:g}k" if v >= 1000 else f"{v:g}"


def marker(kind, x, y, title):
    t = f"<title>{title}</title>" if title else ""
    if kind == "dp":
        return f'<circle class="dp m" cx="{x:.1f}" cy="{y:.1f}" r="5">{t}</circle>'
    if kind == "uv":
        return f'<rect class="uv m" x="{x - 4.5:.1f}" y="{y - 4.5:.1f}" width="9" height="9" rx="1.5">{t}</rect>'
    if kind == "mj":
        return f'<path class="mj m" d="M{x:.1f},{y - 6:.1f} l6,10 l-12,0 z">{t}</path>'
    if kind == "ci":
        return f'<path class="ci m" d="M{x:.1f},{y + 6:.1f} l6,-10 l-12,0 z">{t}</path>'
    if kind == "bi":
        return (f'<path class="bi m" d="M{x - 2:.1f},{y - 6:.1f} h4 v4 h4 v4 h-4 v4 h-4 v-4 h-4 v-4 h4 z">'
                f'{t}</path>')
    return f'<path class="up m" d="M{x:.1f},{y - 6:.1f} l6,6 l-6,6 l-6,-6 z">{t}</path>'


def legend(keys):
    # one row of entries, wrapping before the right edge; returns svg parts and rows used
    out, x, y, rows = [], 21.0, 47.0, 1
    for k in keys:
        width = 30 + 6.6 * len(SERIES[k])
        if x + width > W - 16:
            x, y, rows = 21.0, y + 20, rows + 1
        out.append(marker(k, x, y, None))
        out.append(f'<text class="t3" x="{x + 10:.1f}" y="{y + 4:.0f}">{SERIES[k]}</text>')
        x += width + 14
    return out, rows


def draw(fname, label, DATA, FOOTNOTES, keys):
    allv = [v for rows in DATA.values() for r in rows for s in r for v in s if v is not None]
    lo, hi, tk = axis(allv)
    scale = (X1 - X0) / math.log10(hi / lo)

    def X(v):
        return X0 + math.log10(v / lo) * scale

    # one lane per series inside each size row, 5 px apart
    lane = {k: (i - (len(keys) - 1) / 2) * 5 for i, k in enumerate(keys)}
    ROW = 8 + 5 * len(keys)
    leg, leg_rows = legend(keys)
    top = 78 + 20 * (leg_rows - 1)
    block = 10 + len(SIZES) * ROW + 13  # panel title to its axis labels
    last_axis = top + (len(DATA) - 1) * (block + HEAD) + block
    height = last_axis + 23 + 15 * (len(FOOTNOTES) - 1) + 13
    others = [NAMES[k] for k in keys if k in NAMES]
    vs = ", ".join(others[:-1]) + " and " + others[-1] if len(others) > 1 else others[0]
    css_vars = ";".join(f"--{k}:{COLOURS[k][0]}" for k in keys)
    css_dark = ";".join(f"--{k}:{COLOURS[k][1]}" for k in keys)
    o = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {height}" width="{W}" height="{height}" role="img" '
        f'aria-label="Round-trip p99.9 latency on {label}, DataPath over shared memory versus {vs}, idle and under stress, 16 B to 1 MiB"><style>\n'
        f"svg{{--bg:#fcfcfb;--t1:#0b0b0b;--t2:#52514e;--grid:#e6e5e1;{css_vars}}}\n"
        f"@media (prefers-color-scheme: dark){{svg{{--bg:#1a1a19;--t1:#f0efec;--t2:#c3c2b7;--grid:#34332f;{css_dark}}}}}\n"
        '.bg{fill:var(--bg)} text{font-family:-apple-system,"Segoe UI",Helvetica,Arial,sans-serif}\n'
        ".t1{font-size:16px;font-weight:600;fill:var(--t1)} .t2{font-size:13px;font-weight:600;fill:var(--t1)}\n"
        ".t3{font-size:12px;fill:var(--t2)} .t4{font-size:11px;fill:var(--t2);font-variant-numeric:tabular-nums}\n"
        ".grid{stroke:var(--grid);stroke-width:1} .wk{stroke-width:1.5;stroke-linecap:round;opacity:.55}\n"
        ".m{stroke:var(--bg);stroke-width:2} " + " ".join(f".{k}{{fill:var(--{k})}}" for k in keys) + "\n"
        + " ".join(f".wk.{k}{{stroke:var(--{k})}}" for k in keys) + "\n"
        f'</style><rect class="bg" width="{W}" height="{height}" rx="6"/>',
        f'<text class="t1" x="16" y="28">{label}: round-trip latency, p99.9 (µs, log scale, lower is better)</text>',
    ] + leg
    y = top
    for name, rows in DATA.items():
        assert len(rows) == len(SIZES), name
        o.append(f'<text class="t2" x="16" y="{y}">{name}</text>')
        g0, g1 = y + 10, y + 10 + len(SIZES) * ROW
        for v in tk:
            o.append(f'<line class="grid" x1="{X(v):.1f}" x2="{X(v):.1f}" y1="{g0}" y2="{g1}"/>')
        for i, (size, vals) in enumerate(zip(SIZES, rows)):
            assert len(vals) == len(keys), (name, size)
            my = g0 + i * ROW + ROW / 2
            o.append(f'<text class="t3" x="70" y="{my + 4:.1f}" text-anchor="end">{size}</text>')
            order = [k for k in DRAW_ORDER if k in keys]
            for k in order:
                med, lo_v, hi_v = vals[keys.index(k)]
                if med is None:
                    continue
                wy = my + lane[k]
                o.append(f'<line class="wk {k}" x1="{X(lo_v):.1f}" x2="{X(hi_v):.1f}" y1="{wy:.1f}" y2="{wy:.1f}"/>')
            for k in order:
                med, lo_v, hi_v = vals[keys.index(k)]
                if med is None:
                    continue
                o.append(marker(k, X(med), my + lane[k], f"{name}, {size}, {SERIES[k]}: {med:g} µs (runs {lo_v:g} to {hi_v:g})"))
        ay = g1 + 13
        for v in tk:
            o.append(f'<text class="t4" x="{X(v):.1f}" y="{ay}" text-anchor="middle">{fmt(v)}</text>')
        y = ay + HEAD
    y = y - HEAD + 23
    for line in FOOTNOTES:
        o.append(f'<text class="t4" x="16" y="{y}">{line}</text>')
        y += 15
    o.append("</svg>\n")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), fname)
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(o))


def main():
    for fname, c in CHARTS.items():
        draw(fname, c["label"], c["panels"], c["footnotes"], c["series"])


if __name__ == "__main__":
    main()
