#!/usr/bin/env python3
"""Folds `perf script` call stacks and renders them as a flame graph SVG.

  perf script -i perf.data | scripts/flamegraph.py --title "..." > flame.svg
  scripts/flamegraph.py --folded folded.txt < perf-script.txt > flame.svg

Standard library only. Each sample's stack is read leaf-first from perf script, reversed to
root-first, and counted; frames are drawn bottom-up with widths proportional to samples.
Hovering a frame shows its sample count and share (an SVG <title>).
"""
import argparse
import collections
import html
import re
import sys
import zlib

# "    ffffaaaa1234 symbol+0x1c (/path/to/dso)"
FRAME_RE = re.compile(r"^\s+[0-9a-f]+\s+(.+?)\s+\((.*)\)\s*$")


def clean(symbol: str, dso: str) -> str:
    symbol = re.sub(r"\+0x[0-9a-f]+$", "", symbol)
    if symbol == "[unknown]":
        symbol = f"[{dso.rsplit('/', 1)[-1]}]"
    # Shorten C++ names: drop argument lists and the project namespace prefix.
    depth, out = 0, []
    for ch in symbol:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0:
            out.append(ch)
    name = "".join(out).strip() or symbol
    name = name.replace("softswitch::", "")
    # Mark kernel frames by where perf found them, as FlameGraph does with --kernel.
    # Kernel image and modules ("[veth]"); "[unknown]" and "[vdso]" are user space.
    is_kernel = ("kernel.kallsyms" in dso or "/lib/modules/" in dso or ".ko" in dso
                 or (dso.startswith("[") and dso not in ("[unknown]", "[vdso]")))
    return name + "_[k]" if is_kernel else name


def fold(lines):
    stacks = collections.Counter()
    frames: list[str] = []
    for line in lines:
        if not line.strip():
            if frames:
                stacks[";".join(reversed(frames))] += 1
                frames = []
            continue
        m = FRAME_RE.match(line)
        if m:
            frames.append(clean(m.group(1), m.group(2)))
    if frames:
        stacks[";".join(reversed(frames))] += 1
    return stacks


def color(name: str, kernel: bool) -> str:
    h = zlib.crc32(name.encode()) & 0xFFFF
    if kernel:  # kernel frames in orange, user frames in blue-green, as in the classic tool
        return f"rgb({205 + h % 50},{110 + h % 70},{40 + h % 30})"
    return f"rgb({50 + h % 40},{140 + h % 60},{170 + h % 60})"


def render(stacks, title: str, width: int = 1200) -> str:
    root = {"n": 0, "kids": {}}
    for stack, count in stacks.items():
        root["n"] += count
        node = root
        for frame in stack.split(";"):
            node = node["kids"].setdefault(frame, {"n": 0, "kids": {}})
            node["n"] += count
    total = root["n"] or 1

    def depth(node):
        return 1 + max((depth(k) for k in node["kids"].values()), default=0)

    row, pad, top = 17, 10, 40
    levels = depth(root)
    height = top + levels * row + 30
    scale = (width - 2 * pad) / total
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" font-family="monospace" font-size="11">',
           f'<rect width="{width}" height="{height}" fill="#fdfdf6"/>',
           f'<text x="{width / 2}" y="22" text-anchor="middle" font-size="15" font-family="sans-serif">'
           f'{html.escape(title)}</text>',
           f'<text x="{pad}" y="{height - 10}" font-family="sans-serif" fill="#555">'
           f'{total} samples. Width is time on CPU; hover for numbers. Kernel frames orange, user frames blue.</text>']

    def draw(name, node, x, level):
        w = node["n"] * scale
        y = height - 30 - (level + 1) * row
        if level >= 0 and w >= 0.3:
            pct = 100.0 * node["n"] / total
            is_kernel = name.endswith("_[k]")
            name = name.removesuffix("_[k]")
            out.append(f'<g><title>{html.escape(name)}: {node["n"]} samples ({pct:.2f}%)</title>'
                       f'<rect x="{x:.2f}" y="{y}" width="{max(w - 0.5, 0.1):.2f}" height="{row - 1}" '
                       f'fill="{color(name, is_kernel)}" rx="1"/>')
            chars = int((w - 6) / 6.6)
            if chars >= 3:
                label = name if len(name) <= chars else name[: chars - 2] + ".."
                out.append(f'<text x="{x + 3:.2f}" y="{y + row - 5}">{html.escape(label)}</text>')
            out.append("</g>")
        cx = x
        for kid_name, kid in sorted(node["kids"].items()):
            draw(kid_name, kid, cx, level + 1)
            cx += kid["n"] * scale

    draw("all", root, pad, -1)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--title", default="Flame graph")
    ap.add_argument("--folded", help="also write the folded stacks here")
    ap.add_argument("--width", type=int, default=1200)
    args = ap.parse_args()
    stacks = fold(sys.stdin)
    if args.folded:
        with open(args.folded, "w") as f:
            for stack, count in stacks.most_common():
                f.write(f"{stack} {count}\n")
    sys.stdout.write(render(stacks, args.title, args.width))
    return 0


if __name__ == "__main__":
    sys.exit(main())
