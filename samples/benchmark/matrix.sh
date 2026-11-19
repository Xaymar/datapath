#!/bin/sh
# Runs the DataPath benchmark for every option combination, 5 runs each,
# and writes everything to benchmark-results.txt in the current directory.
# Usage: samples/benchmark/matrix.sh [path/to/datapath-benchmark] [runs]
BENCH="${1:-$(dirname "$0")/../../build/samples/datapath-benchmark}"
RUNS="${2:-5}"
OUT=benchmark-results.txt
[ -x "$BENCH" ] || { echo "Benchmark not found: $BENCH"; exit 1; }
echo "DataPath benchmark matrix, $RUNS runs per size" > "$OUT"
for opts in "" "--yield" "--spin 0" "--spin 200" "--threads 2" "--clients 8" "--clients 8 --threads 4" "--load --threads 2"; do
	printf '\n==== %s\n' "${opts:-defaults}" >> "$OUT"
	echo "Running: ${opts:-defaults}"
	# shellcheck disable=SC2086
	"$BENCH" local --runs "$RUNS" $opts >> "$OUT" 2>&1
done
echo "Results written to $OUT"
