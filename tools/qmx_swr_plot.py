#!/usr/bin/env python3
"""Plot a qmx_swr_sweep.py --csv as a standalone SVG, one panel per band (no dependencies).

  python3 tools/qmx_swr_plot.py sweep.csv -o sweep.svg --title "EFHW 40m, Tune 25%"

Each panel has its own frequency axis and a shared log SWR axis (1 to 10). The dashed line is
SWR 2; points at or below it are green, above it amber. Open the SVG in a browser.
"""
import argparse
import csv
import math

PW, PH = 330, 215           # panel size
L, R, T, B = 44, 10, 34, 34  # margins inside a panel
COLS = 3
YMAX = 10.0
GOOD, BAD, LINE = "#15803d", "#b45309", "#6b7280"
ORDER = ["160m", "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"]


def panel(name, pts, ox, oy):
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    pad = (x1 - x0) * 0.04 or 0.01
    x0, x1 = x0 - pad, x1 + pad

    def X(mhz): return ox + L + (mhz - x0) / (x1 - x0) * (PW - L - R)
    def Y(swr): return oy + T + (1 - math.log(min(max(swr, 1.0), YMAX)) / math.log(YMAX)) * (PH - T - B)

    o = [f'<rect x="{ox + L}" y="{oy + T}" width="{PW - L - R}" height="{PH - T - B}" fill="#fafafa" stroke="#d1d5db"/>']
    for v in (1, 2, 3, 5, 10):
        y = Y(v)
        o.append(f'<line x1="{ox + L}" x2="{ox + PW - R}" y1="{y:.1f}" y2="{y:.1f}" stroke="#e5e7eb"/>')
        o.append(f'<text x="{ox + L - 5}" y="{y + 4:.1f}" text-anchor="end" fill="#374151">{v}</text>')
    y2 = Y(2)
    o.append(f'<line x1="{ox + L}" x2="{ox + PW - R}" y1="{y2:.1f}" y2="{y2:.1f}" stroke="{LINE}" stroke-dasharray="4 3"/>')
    span = x1 - x0
    step = next(s for s in (0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10) if span / s <= 5)
    t = math.ceil(x0 / step) * step
    while t <= x1 + 1e-9:
        o.append(f'<text x="{X(t):.1f}" y="{oy + PH - B + 14}" text-anchor="middle" fill="#374151">{t:g}</text>')
        t += step
    d = " ".join(f"{'M' if i == 0 else 'L'}{X(x):.1f},{Y(s):.1f}" for i, (x, s) in enumerate(pts))
    o.append(f'<path d="{d}" fill="none" stroke="{LINE}" stroke-width="1.2"/>')
    for x, s in pts:
        o.append(f'<circle cx="{X(x):.1f}" cy="{Y(s):.1f}" r="2.6" fill="{GOOD if s <= 2 else BAD}"/>')
    lo = min(pts, key=lambda p: p[1])
    frac = sum(1 for _, s in pts if s <= 2) / len(pts)
    o.append(f'<text x="{ox + L}" y="{oy + 14}" font-size="13" font-weight="bold">{name}</text>')
    o.append(f'<text x="{ox + PW - R}" y="{oy + 14}" text-anchor="end" fill="#374151">min {lo[1]:.2f} @ {lo[0]:.3f}</text>')
    o.append(f'<text x="{ox + PW - R}" y="{oy + 28}" text-anchor="end" fill="#374151">{frac:.0%} of points at or below 2</text>')
    o.append(f'<text x="{ox + (L + PW - R) / 2}" y="{oy + PH - 4}" text-anchor="middle" fill="#6b7280">MHz</text>')
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("-o", "--out", default="swr.svg")
    ap.add_argument("--title", default="")
    a = ap.parse_args()
    bands = {}
    with open(a.csv) as f:
        for r in csv.DictReader(f):
            if r["swr_median"]:
                bands.setdefault(r["band"], []).append((int(r["freq_hz"]) / 1e6, float(r["swr_median"])))
    if not bands:
        raise SystemExit("no readings in the CSV")
    names = [n for n in ORDER if n in bands] + [n for n in bands if n not in ORDER]
    rows = math.ceil(len(names) / COLS)
    W, H = COLS * PW, 44 + rows * PH
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="sans-serif" font-size="11">',
         f'<rect width="{W}" height="{H}" fill="#fff"/>',
         f'<text x="10" y="24" font-size="16" font-weight="bold">QMX SWR sweep {a.title}</text>']
    for i, n in enumerate(names):
        o += panel(n, sorted(bands[n]), (i % COLS) * PW, 44 + (i // COLS) * PH)
    o.append("</svg>")
    with open(a.out, "w") as f:
        f.write("\n".join(o))
    print(f"wrote {a.out}: {len(names)} bands, {sum(len(v) for v in bands.values())} points")


if __name__ == "__main__":
    main()
