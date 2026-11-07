#!/usr/bin/env bash
# Runs every unit-test binary under Valgrind memcheck. Writes XML + text logs and a Markdown
# summary to reports/valgrind/. Exits non-zero on any memory error or definite/indirect leak.
# Usage: scripts/valgrind.sh [BUILD_DIR]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
BUILD_DIR=${1:-build/dev}
OUT=reports/valgrind
rm -rf "$OUT" && mkdir -p "$OUT"

# Valgrind serializes threads, so the lock-free stress tests (which spin waiting for the other
# thread) take minutes and prove nothing about memory. ThreadSanitizer covers them in CI.
GTEST_FILTER="-*Concurrent*:*TwoThreadStress*"

mapfile -t bins < <(find "$BUILD_DIR/tests" -maxdepth 1 -type f -executable -name '*_tests' | sort)
[[ ${#bins[@]} -gt 0 ]] || { echo "no test binaries in $BUILD_DIR/tests" >&2; exit 2; }

echo "$(valgrind --version) on ${#bins[@]} test binaries"
status=0
{
  echo "## Valgrind memcheck"
  echo
  echo "| Binary | Result | Errors | Definitely lost (bytes) |"
  echo "|---|---|---|---|"
} > "$OUT/summary.md"

for bin in "${bins[@]}"; do
  name=$(basename "$bin")
  valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect --track-origins=yes --error-exitcode=99 \
    --xml=yes --xml-file="$OUT/$name.xml" --log-file="$OUT/$name.log" \
    "$bin" --gtest_brief=1 --gtest_filter="$GTEST_FILTER" > "$OUT/$name.stdout" 2>&1
  rc=$?
  errors=$(grep -c '<error>' "$OUT/$name.xml" || true)
  lost=$(grep -oP 'definitely lost: \K[0-9,]+' "$OUT/$name.log" | tr -d , || echo 0)
  if [[ $rc -eq 0 ]]; then result="pass"; else result="FAIL (rc=$rc)"; status=1; fi
  echo "| \`$name\` | $result | $errors | ${lost:-0} |" >> "$OUT/summary.md"
done

cat "$OUT/summary.md"
exit $status
