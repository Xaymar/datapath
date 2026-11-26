#!/bin/sh
# Runs the DataPath benchmark for every option combination, 5 runs each,
# and writes everything to benchmark-results.txt in the current directory.
# Usage: samples/benchmark/matrix.sh [path/to/datapath-benchmark] [runs]
BENCH="${1:-$(dirname "$0")/../../build/samples/datapath-benchmark}"
RUNS="${2:-5}"
OUT=benchmark-results.txt
[ -x "$BENCH" ] || { echo "Benchmark not found: $BENCH"; exit 1; }
echo "DataPath benchmark matrix, $RUNS runs per size" > "$OUT"
for opts in "" "--yield" "--spin 0" "--spin 200" "--threads 2" "--threads auto" "--clients 8" "--clients 8 --threads 4" "--clients 8 --threads auto" "--load --threads 2" "--stress 0" "--stress 0 --spin 0"; do
	printf '\n==== %s\n' "${opts:-defaults}" >> "$OUT"
	echo "Running: ${opts:-defaults}"
	# shellcheck disable=SC2086
	"$BENCH" local --runs "$RUNS" $opts >> "$OUT" 2>&1
done
# The same core through the C API (datapath.h). The Rust core is compared
# against these runs, since it is measured through the same interface.
CAPI="$(dirname "$BENCH")/datapath-benchmark-c"
if [ -x "$CAPI" ]; then
	for opts in "" "--clients 8 --threads auto" "--load --threads 2" "--stress 0"; do
		printf '\n==== c-api %s\n' "${opts:-defaults}" >> "$OUT"
		echo "Running: c-api ${opts:-defaults}"
		# shellcheck disable=SC2086
		"$CAPI" local --runs "$RUNS" $opts >> "$OUT" 2>&1
	done
fi
LIBUV="$(dirname "$BENCH")/libuv-benchmark"
if [ -x "$LIBUV" ]; then
	for opts in "--spin 0" "" "--stress 0 --spin 0" "--stress 0"; do
		printf '\n==== libuv %s\n' "${opts:-defaults}" >> "$OUT"
		echo "Running: libuv ${opts:-defaults}"
		# shellcheck disable=SC2086
		"$LIBUV" local --runs "$RUNS" $opts >> "$OUT" 2>&1
	done
fi
# Shared-memory competitors: as shipped and with DataPath's spin.
for other in cppipc boost; do
	EXE="$(dirname "$BENCH")/$other-benchmark"
	[ -x "$EXE" ] || continue
	for opts in "--spin 0 --no-mmcss --priority 0" "" "--stress 0 --spin 0 --no-mmcss --priority 0" "--stress 0"; do
		printf '\n==== %s %s\n' "$other" "${opts:-defaults}" >> "$OUT"
		echo "Running: $other ${opts:-defaults}"
		# shellcheck disable=SC2086
		"$EXE" local --runs "$RUNS" $opts >> "$OUT" 2>&1
	done
done
echo "Results written to $OUT"
