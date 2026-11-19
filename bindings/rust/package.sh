#!/bin/sh
# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copy the C++ sources and the license into the crates so they are
# self-contained, then run `cargo package` (or the cargo command given, e.g.
# `./package.sh publish --dry-run`). The copies are ignored by git.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
vendor="$here/datapath-sys/vendor"

rm -rf "$vendor"
mkdir -p "$vendor"
cp "$root/CMakeLists.txt" "$root/LICENSE" "$vendor/"
cp -R "$root/cmake" "$root/include" "$root/source" "$vendor/"
cp "$root/LICENSE" "$here/datapath-sys/LICENSE"
cp "$root/LICENSE" "$here/datapath/LICENSE"

cd "$here"
if [ "$#" -eq 0 ]; then
	set -- package --workspace
fi
exec cargo "$@" --allow-dirty
