#!/usr/bin/env python3
# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

"""Check that every package carries the version set in CMakeLists.txt.

    check_versions.py [--tag vX.Y.Z]   exit 1 on any mismatch
    check_versions.py --print          print the version and exit

The Python package reads its version from CMakeLists.txt, so it can't drift.
"""

import argparse
import json
import pathlib
import re
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    tomllib = None

ROOT = pathlib.Path(__file__).resolve().parents[2]


def cmake_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"set\(DATAPATH_VERSION_{name}\s+(\d+)\)", text)
        if not m:
            sys.exit(f"CMakeLists.txt: DATAPATH_VERSION_{name} not found")
        parts.append(m.group(1))
    return ".".join(parts)


def toml_value(path: pathlib.Path, *keys: str) -> str:
    text = path.read_text(encoding="utf-8")
    if tomllib:
        data = tomllib.loads(text)
        for key in keys:
            data = data[key]
        return data
    # Fallback: good enough for the simple tables used here.
    m = re.search(rf'^\s*{re.escape(keys[-1])}\s*=\s*"([^"]+)"', text, re.M)
    return m.group(1) if m else "<missing>"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", help="release tag that must match, e.g. v1.0.0 or refs/tags/v1.0.0")
    parser.add_argument("--print", action="store_true", help="print the version and exit")
    args = parser.parse_args()

    version = cmake_version()
    if args.print:
        print(version)
        return 0

    rust = ROOT / "bindings/rust"
    sys_dep = toml_value(rust / "datapath/Cargo.toml", "dependencies", "sys", "version")
    found = {
        "CMakeLists.txt": version,
        "bindings/node/package.json": json.loads(
            (ROOT / "bindings/node/package.json").read_text(encoding="utf-8"))["version"],
        "bindings/node/package-lock.json": json.loads(
            (ROOT / "bindings/node/package-lock.json").read_text(encoding="utf-8"))["version"],
        "bindings/rust/Cargo.toml": toml_value(rust / "Cargo.toml", "workspace", "package", "version"),
        "bindings/rust/datapath/Cargo.toml (sys dependency)": sys_dep.lstrip("="),
    }
    if args.tag:
        found["tag"] = args.tag.removeprefix("refs/tags/").removeprefix("v")

    bad = {where: v for where, v in found.items() if v != version}
    for where, v in found.items():
        print(f"{'ok ' if v == version else 'BAD'} {v:<12} {where}")
    if not sys_dep.startswith("="):
        print("BAD sys dependency must be pinned with '=': " + sys_dep)
        return 1
    if bad:
        print(f"\nversion mismatch: expected {version} everywhere", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
