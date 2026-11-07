#!/usr/bin/env bash
# Static analysis with Cppcheck. Writes XML + HTML reports to reports/cppcheck/ and exits
# non-zero if anything was found.
# Usage: scripts/cppcheck.sh [BUILD_DIR]
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR=${1:-build/dev}
OUT=reports/cppcheck
rm -rf "$OUT" && mkdir -p "$OUT"
[[ -f "$BUILD_DIR/compile_commands.json" ]] || { echo "no compile_commands.json in $BUILD_DIR" >&2; exit 2; }

echo "$(cppcheck --version)"
cppcheck \
  --project="$BUILD_DIR/compile_commands.json" \
  --file-filter="src/*" \
  --enable=warning,style,performance,portability \
  --inline-suppr \
  --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression \
  --suppress=useStlAlgorithm \
  --suppress="*:/usr/*" \
  -j "$(nproc)" \
  --xml --output-file="$OUT/cppcheck.xml" \
  --quiet

cppcheck-htmlreport --file="$OUT/cppcheck.xml" --report-dir="$OUT/html" \
  --source-dir=. --title="softswitch Cppcheck" >/dev/null

python3 - "$OUT/cppcheck.xml" "$OUT/summary.md" <<'PY'
import sys, xml.etree.ElementTree as ET
errors = ET.parse(sys.argv[1]).getroot().findall("./errors/error")
lines = ["## Cppcheck", "", f"**{len(errors)} finding(s)**", ""]
if errors:
    lines += ["| Severity | ID | Location | Message |", "|---|---|---|---|"]
    for e in errors:
        loc = e.find("location")
        where = f"{loc.get('file')}:{loc.get('line')}" if loc is not None else "-"
        lines.append(f"| {e.get('severity')} | `{e.get('id')}` | `{where}` | {e.get('msg')} |")
open(sys.argv[2], "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(1 if errors else 0)
PY
