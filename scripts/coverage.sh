#!/usr/bin/env bash
# Builds the coverage preset, runs unit tests and writes a gcovr report to reports/coverage/.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=reports/coverage
rm -rf "$OUT" && mkdir -p "$OUT"

cmake --preset coverage >/dev/null
cmake --build --preset coverage
find build/coverage -name '*.gcda' -delete
ctest --preset coverage

gcovr --root . --object-directory build/coverage \
  --filter 'src/' --filter 'include/' \
  --exclude-unreachable-branches --exclude-throw-branches \
  --html-details "$OUT/index.html" \
  --xml "$OUT/cobertura.xml" \
  --txt "$OUT/summary.txt" \
  --print-summary | tee "$OUT/summary-short.txt"
