#!/usr/bin/env python3
"""Turns reports/bench/results.jsonl into Markdown tables and an SVG latency chart.

  scripts/bench_report.py [RESULTS_DIR] [SVG_OUT]

Standard library only, so it runs on the Pi and in CI.
"""
import csv
import json
import math
import pathlib
import sys


def load(path: pathlib.Path):
    if not path.exists():
        return []
    return [json.loads(l) for l in path.read_text().splitlines() if l.strip()]


def ndr_table(rows) -> str:
    """No-drop rate per path and frame size, with the 64-byte gain over the first path."""
    sizes = sorted({r["size"] for r in rows})
    labels = list(dict.fromkeys(r["label"] for r in rows))
    by = {(r["label"], r["size"]): r["ndr_pps"] for r in rows}
    out = ["| Path | " + " | ".join(f"{s} B" for s in sizes) + " |", "|---|" + "---|" * len(sizes)]
    for label in labels:
        cells = [fmt_pps(by[(label, s)]) if (label, s) in by else "–" for s in sizes]
        out.append(f"| {label} | " + " | ".join(cells) + " |")
    return "\n".join(out)


def fmt_pps(v: int) -> str:
    return f"{v / 1e6:.2f} Mpps" if v >= 1e6 else f"{v / 1e3:.0f} kpps"


def throughput_table(rows) -> str:
    out = ["| Path | Frame | Offered | Forwarded | Loss | Lost in switch RX ring | Forwarded Mb/s |",
           "|---|---|---|---|---|---|---|"]
    for r in rows:
        if r["rate"] != 0:
            continue
        sent, got = r["gen"]["sent"], r["sink"]["received"]
        loss = 100.0 * (sent - got) / sent if sent else 0.0
        mbps = r["sink"]["pps"] * r["size"] * 8 / 1e6
        ring = r.get("switch", {}).get("switch_ring_drops", 0)
        ring_share = f"{100.0 * ring / (sent - got):.0f} % of loss" if sent > got else "–"
        out.append(f"| {r['label']} | {r['size']} B | {fmt_pps(r['gen']['pps'])} | {fmt_pps(r['sink']['pps'])} "
                   f"| {loss:.1f} % | {ring_share} | {mbps:.0f} |")
    return "\n".join(out)


def latency_table(rows) -> str:
    out = ["| Path | Rate | p50 | p90 | p99 | p99.9 | max |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        if r["rate"] == 0:
            continue
        lat = r["sink"]["lat_us"]
        out.append(f"| {r['label']} | {fmt_pps(r['rate'])} | {lat['p50']:.1f} µs | {lat['p90']:.1f} µs | "
                   f"{lat['p99']:.1f} µs | {lat['p999']:.1f} µs | {lat['max']:.0f} µs |")
    return "\n".join(out)


def svg_histograms(results_dir: pathlib.Path, svg_out: pathlib.Path) -> None:
    """Latency tail chart: for each latency CSV, the share of packets slower than x (a CCDF),
    log-log, so p50 and p99.9 are both readable and a heavy tail stands out."""
    series = []
    palette = ["#2f6fde", "#d64545", "#1f9d55", "#d69e2e"]
    for i, path in enumerate(sorted(results_dir.glob("latency-*.csv"))):
        with path.open() as f:
            pts = [(float(r["upper_us"]), int(r["count"])) for r in csv.DictReader(f)]
        total = sum(c for _, c in pts)
        if total == 0:
            continue
        ccdf, seen = [], 0
        for x, c in pts:
            seen += c
            ccdf.append((x, (total - seen) / total))
        series.append((path.stem.replace("latency-", ""), ccdf, total, palette[i % len(palette)]))
    if not series:
        return
    w, h, ml, mb, mt, mr = 760, 380, 70, 50, 20, 170
    xmin = 1.0
    xmax = 10 ** math.ceil(math.log10(max(x for _, pts, _, _ in series for x, _ in pts)))
    ymin = 10 ** math.floor(math.log10(1 / max(t for _, _, t, _ in series)))
    lx = lambda x: (math.log10(max(x, xmin)) - math.log10(xmin)) / (math.log10(xmax) - math.log10(xmin))
    ly = lambda y: (math.log10(max(y, ymin)) - math.log10(ymin)) / (0 - math.log10(ymin))
    sx = lambda x: ml + lx(x) * (w - ml - mr)
    sy = lambda y: h - mb - ly(y) * (h - mb - mt)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" font-family="sans-serif" font-size="12">',
             f'<rect width="{w}" height="{h}" fill="#ffffff"/>']
    decade = xmin
    while decade <= xmax:
        x = sx(decade)
        label = f"{decade:g} µs" if decade < 1000 else f"{decade / 1000:g} ms"
        parts.append(f'<line x1="{x:.1f}" y1="{mt}" x2="{x:.1f}" y2="{h - mb}" stroke="#eee"/>')
        parts.append(f'<text x="{x:.1f}" y="{h - mb + 16}" text-anchor="middle" fill="#333">{label}</text>')
        decade *= 10
    y = 1.0
    while y >= ymin:
        yy = sy(y)
        pct = f"{100 * y:g} %"
        parts.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{w - mr}" y2="{yy:.1f}" stroke="#eee"/>')
        parts.append(f'<text x="{ml - 6}" y="{yy + 4:.1f}" text-anchor="end" fill="#333">{pct}</text>')
        y /= 10
    parts.append(f'<line x1="{ml}" y1="{h - mb}" x2="{w - mr}" y2="{h - mb}" stroke="#555"/>')
    parts.append(f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{h - mb}" stroke="#555"/>')
    parts.append(f'<text x="{(ml + w - mr) / 2}" y="{h - 12}" text-anchor="middle" fill="#333">one-way latency</text>')
    parts.append(f'<text x="16" y="{(mt + h - mb) / 2}" transform="rotate(-90 16 {(mt + h - mb) / 2})" '
                 f'text-anchor="middle" fill="#333">packets slower than this</text>')
    for idx, (name, pts, total, color) in enumerate(series):
        # Step plot: the share stays constant until the next bucket boundary.
        d, prev = [], 1.0
        for j, (x, y) in enumerate(pts):
            d.append(f"{'M' if j == 0 else 'L'}{sx(x):.1f},{sy(prev):.1f}")
            if y <= 0:
                d.append(f"L{sx(x):.1f},{sy(ymin):.1f}")
                break
            d.append(f"L{sx(x):.1f},{sy(y):.1f}")
            prev = y
        parts.append(f'<path d="{" ".join(d)}" fill="none" stroke="{color}" stroke-width="2"/>')
        ly_ = mt + 18 * idx + 10
        parts.append(f'<line x1="{w - mr + 12}" y1="{ly_}" x2="{w - mr + 32}" y2="{ly_}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<text x="{w - mr + 38}" y="{ly_ + 4}" fill="#333">{name}</text>')
    parts.append("</svg>")
    svg_out.write_text("\n".join(parts) + "\n")


def main() -> int:
    results_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "reports/bench")
    svg_out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else results_dir / "latency.svg")
    rows = load(results_dir / "results.jsonl")
    ndr = load(results_dir / "ndr.jsonl")
    if ndr:
        print("### No-drop rate (highest offered rate with < 0.01 % loss)\n")
        print(ndr_table(ndr))
        print()
    if any(r["rate"] == 0 for r in rows):
        print("### Throughput (maximum offered rate)\n")
        print(throughput_table(rows))
        print()
    print("### Latency (fixed offered rate, 64-byte frames)\n")
    print(latency_table(rows))
    svg_histograms(results_dir, svg_out)
    print(f"\n(chart: {svg_out})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
