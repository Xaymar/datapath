#!/usr/bin/env python3
# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

"""Summarize one or more Cobertura XML reports as a Markdown table.

Works with both gcovr (Linux) and OpenCppCoverage (Windows) output. Reports can
list the same file several times (OpenCppCoverage emits one entry per test
executable), so line hits are merged per file before counting.

Usage: coverage_summary.py --root <repo> --title <text> report.xml [...]
       [--min <percent>]   fail if total line coverage is below this
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET


def normalize(path, root):
    path = path.replace("\\", "/")
    root = root.replace("\\", "/").rstrip("/")
    # Drop drive letters; OpenCppCoverage puts the drive in <source> and the rest in filename.
    path = re.sub(r"^[A-Za-z]:", "", path)
    root = re.sub(r"^[A-Za-z]:", "", root)
    if root and path.lower().startswith(root.lower() + "/"):
        return path[len(root) + 1 :]
    # Fall back to the first top-level project directory in the path.
    m = re.search(r"(?:^|/)((?:source|include)/.*)$", path)
    return m.group(1) if m else path.lstrip("/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reports", nargs="+")
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--title", default="Coverage")
    ap.add_argument("--min", type=float, default=0.0)
    args = ap.parse_args()

    files = {}  # path -> {line: hits}
    for report in args.reports:
        tree = ET.parse(report)
        for cls in tree.iter("class"):
            name = normalize(cls.get("filename", ""), args.root)
            if not (name.startswith("source/") or name.startswith("include/")):
                continue
            lines = files.setdefault(name, {})
            for line in cls.iter("line"):
                num = int(line.get("number"))
                lines[num] = max(lines.get(num, 0), int(line.get("hits", "0")))

    total_valid = total_covered = 0
    rows = []
    for name in sorted(files):
        lines = files[name]
        valid = len(lines)
        covered = sum(1 for h in lines.values() if h > 0)
        total_valid += valid
        total_covered += covered
        pct = 100.0 * covered / valid if valid else 100.0
        rows.append(f"| `{name}` | {covered} / {valid} | {pct:.1f}% |")

    total = 100.0 * total_covered / total_valid if total_valid else 0.0
    out = [
        f"### {args.title}: {total:.1f}% lines",
        "",
        "<details><summary>Per file</summary>",
        "",
        "| File | Lines | Coverage |",
        "| --- | ---: | ---: |",
        *rows,
        f"| **Total** | **{total_covered} / {total_valid}** | **{total:.1f}%** |",
        "",
        "</details>",
        "",
    ]
    text = "\n".join(out)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")

    if total_valid == 0:
        print("error: no project files found in the coverage report(s)", file=sys.stderr)
        return 1
    if total < args.min:
        print(f"error: line coverage {total:.1f}% is below the minimum {args.min:.1f}%", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
