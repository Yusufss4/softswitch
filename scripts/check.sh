#!/usr/bin/env bash
# Runs the local equivalent of the CI quality gates. Use before opening a PR.
# Usage: scripts/check.sh [--quick]   (--quick skips sanitizers, valgrind and fuzzing)
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/.local/bin:$PATH"
QUICK=0
[[ "${1:-}" == "--quick" ]] && QUICK=1

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

step "format (pre-commit)"
git add -N . >/dev/null 2>&1 || true   # make new files visible to pre-commit
pre-commit run --all-files

step "build + unit tests (dev)"
cmake --preset dev >/dev/null
cmake --build --preset dev
ctest --preset dev

step "optimized build (release, catches -O2-only warnings)"
cmake --preset release >/dev/null
cmake --build --preset release

step "clang-tidy"
CLANG_TIDY=${CLANG_TIDY:-$(command -v clang-tidy-18 || echo clang-tidy)} scripts/clang-tidy.sh build/dev

step "cppcheck"
scripts/cppcheck.sh build/dev

if [[ $QUICK -eq 0 ]]; then
  step "ASan + UBSan"
  cmake --preset asan >/dev/null
  cmake --build --preset asan
  ctest --preset asan

  step "valgrind"
  scripts/valgrind.sh build/dev

  if command -v clang++ >/dev/null; then
    step "fuzz (20 s per target)"
    cmake --preset fuzz >/dev/null
    cmake --build --preset fuzz
    for f in build/fuzz/fuzz/fuzz_*; do
      [[ -x "$f" && ! -d "$f" ]] || continue
      "$f" -max_total_time=20 -print_final_stats=0 2>&1 | tail -1
    done
  fi
fi
step "all checks passed"
