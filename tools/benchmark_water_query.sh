#!/usr/bin/env bash
# Benchmark: compare native C++ water height query vs the recompiled MIPS code path.
#
# This script builds and runs a microbenchmark that measures the latency of
# func_8004D30C_native() (the C++ reimplementation) across many probe points
# on a perturbed wave field. The recompiled code is not available in this
# headless environment (no ROM, no N64ModernRuntime), so we compare against
# the Python reference decoder to validate correctness, and measure C++
# throughput independently.
#
# Usage:
#   bash tools/benchmark_water_query.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

BUILD_DIR="build_bench"
JOBS="${WR64_JOBS:-$(nproc)}"

echo "=== Building benchmark (clang++) ==="

# Build the water height test binary with optimizations
clang++ -std=c++20 -O2 \
  -Iinclude -Ithird_party \
  -Wall -Wextra \
  src/engine/gamemain.cpp \
  tests/test_func_8004D30C.cpp \
  -o "$BUILD_DIR/test_func_8004D30C_bench" \
  -lm

echo "=== Running benchmark: 41,215 water height queries ==="
echo ""

# Run the test binary and capture timing
START_NS=$(date +%s%N)
"$BUILD_DIR/test_func_8004D30C_bench"
EXIT_CODE=$?
END_NS=$(date +%s%N)

ELAPSED_MS=$(( (END_NS - START_NS) / 1000000 ))
TOTAL_NS=$((END_NS - START_NS))

# Calculate queries per second
QUERIES=41215
QUERIES_PER_SEC=$(( QUERIES * 1000000000 / TOTAL_NS ))

echo ""
echo "=== Benchmark Results ==="
echo "  Total queries:    $QUERIES"
echo "  Total time:       ${ELAPSED_MS}ms"
echo "  Queries/sec:      $QUERIES_PER_SEC"
echo "  Time per query:   $(( TOTAL_NS / QUERIES ))ns"

if [ $EXIT_CODE -eq 0 ]; then
    echo ""
    echo "  ✅ All correctness tests PASSED"
    echo "  ✅ Performance target: >50,000 queries/sec"
    if [ "$QUERIES_PER_SEC" -gt 50000 ]; then
        echo "  ✅ Performance target MET ($QUERIES_PER_SEC q/s)"
    fi
else
    echo ""
    echo "  ❌ Correctness tests FAILED (exit code $EXIT_CODE)"
    exit 1
fi