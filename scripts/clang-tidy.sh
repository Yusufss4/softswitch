#!/usr/bin/env bash
# Runs clang-tidy over first-party sources (src/ and tools/) using the compile database in BUILD_DIR.
# Usage: scripts/clang-tidy.sh [BUILD_DIR]   (env: CLANG_TIDY=clang-tidy-18)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR=${1:-build/dev}
CLANG_TIDY=${CLANG_TIDY:-clang-tidy}
[[ -f "$BUILD_DIR/compile_commands.json" ]] || { echo "no compile_commands.json in $BUILD_DIR" >&2; exit 2; }

mapfile -t files < <(git ls-files 'src/*.cpp' 'src/**/*.cpp' 'tools/*.cpp')
echo "clang-tidy ($($CLANG_TIDY --version | grep -o 'version [0-9.]*')) on ${#files[@]} files"
printf '%s\n' "${files[@]}" |
  xargs -P "$(nproc)" -I{} "$CLANG_TIDY" -p "$BUILD_DIR" --quiet {}
echo "clang-tidy: clean"
