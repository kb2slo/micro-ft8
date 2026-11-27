#!/usr/bin/env python3
"""Draw the twin-P4 board's two schematic sheets as SVG (no dependencies).

  python3 hardware/twin-p4/draw_schematic.py

Writes power.svg and links.svg beside this script. The drawings are derived: the netlist in
README.md is the source, and a change edits both in the same commit (AGENTS.md, **Prose is
authoritative**). Each SVG follows the viewer's light or dark theme.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

STYLE = """<style>
svg { --bg: #ffffff; --ink: #17211c; --muted: #5a6a62; --fill: #f2f5f3;
      --pwr: #c2410c; --sig: #1d4ed8; --grove: #0f766e; }
@media (prefers-color-scheme: dark) {
  svg { --bg: #151d1a; --ink: #e2ebe6; --muted: #93a39b; --fill: #0f1513;
        --pwr: #fb923c; --sig: #7aa7ff; --grove: #2dd4bf; } }
.bg { fill: var(--bg); }
.w { stroke: var(--ink); stroke-width: 1.6; fill: none; }
.vin { stroke: var(--ink); stroke-width: 2.4; fill: none; }
.pwr { stroke: var(--pwr); stroke-width: 2.4; fill: none; }
.sig { stroke: var(--sig); stroke-width: 2; fill: none; }
.grove { stroke: var(--grove); stroke-width: 2; fill: none; }
.arh { fill: var(--muted); }
.box { fill: var(--fill); stroke: var(--ink); stroke-width: 1.6; }
.part { fill: var(--bg); stroke: var(--ink); stroke-width: 1.6; }
.dot { fill: var(--ink); } .dotp { fill: var(--pwr); }
text { font-family: ui-monospace, Menlo, Consolas, monospace; fill: var(--ink); }
.t { font-size: 13px; } .tr { font-size: 13px; font-weight: 600; }
.ts { font-size: 11.5px; fill: var(--muted); } .tp { font-size: 12px; }
.tb { font-size: 15px; font-weight: 600; } .tn { font-size: 13px; font-weight: 600; fill: var(--pwr); }
.tg { font-size: 12px; font-weight: 600; }
.to { font-size: 11px; font-style: italic; fill: var(--muted); }
.og { opacity: .5; } .og .w, .og .vin, .og .pwr, .og .part { stroke-dasharray: 5 4; }
</style>"""


def gnd(x, y):
    return (L(x - 12, y, x + 12, y) + L(x - 7, y + 5, x + 7, y + 5) + L(x - 2, y + 10, x + 2, y + 10))


def L(x1, y1, x2, y2, c="w"):
    return f'<line class="{c}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}"/>'


def T(x, y, s, c="t", a="middle"):
    return f'<text class="{c}" x="{x}" y="{y}" text-anchor="{a}">{s}</text>'


def dot(x, y, c="dot"):
    return f'<circle class="{c}" cx="{x}" cy="{y}" r="3.5"/>'


def ecap(x, ref, val, wc):
    """Electrolytic capacitor from the rail at y=100 to ground."""
    return (L(x, 100, x, 140, wc) + L(x - 14, 140, x + 14, 140)
            + f'<path class="w" d="M{x - 14},152 Q{x},144 {x + 14},152"/>' + T(x - 20, 137, "+", "ts")
            + L(x, 149, x, 180) + gnd(x, 180) + T(x, 214, ref, "tr") + T(x, 230, val, "ts"))


def ccap(x, ref, val, wc):
    """Ceramic capacitor from the rail at y=100 to ground."""
    return (L(x, 100, x, 140, wc) + L(x - 14, 140, x + 14, 140) + L(x - 14, 150, x + 14, 150)
            + L(x, 150, x, 180) + gnd(x, 180) + T(x, 214, ref, "tr") + T(x, 230, val, "ts"))


def optional(*parts):
    return '<g class="og">' + "".join(parts) + '</g>'


def sheet(w, h, label, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" role="img" aria-label="{label}">'
            + STYLE + f'<rect class="bg" width="{w}" height="{h}"/>' + "".join(body) + '</svg>\n')


def power():
    p = []
    # J1, the supply in, and J2, the pass-through to the QMX, tapped ahead of the fuse.
    p += ['<rect class="box" x="30" y="70" width="80" height="100" rx="4"/>', T(70, 93, "J1", "tr"),
          T(70, 112, "DC in", "ts"), T(70, 128, "9–14 V", "ts"), T(70, 144, "5.5 × 2.1", "ts"),
          T(118, 93, "+", "ts", "start"), L(110, 150, 130, 150), L(130, 150, 130, 170), gnd(130, 170)]
    p += ['<rect class="box" x="30" y="195" width="80" height="85" rx="4"/>', T(70, 218, "J2", "tr"),
          T(70, 237, "to QMX", "ts"), T(70, 253, "(through)", "ts"), T(118, 209, "+", "ts", "start"),
          L(110, 215, 160, 215, "vin"), L(160, 215, 160, 100, "vin"), dot(160, 100),
          L(110, 262, 130, 262), L(130, 262, 130, 272), gnd(130, 272)]
    p += [L(110, 100, 200, 100, "vin"),
          '<rect class="part" x="200" y="90" width="50" height="20" rx="2"/>', L(200, 100, 250, 100, "vin"),
          T(225, 80, "F1", "tr"), T(225, 128, "1 A PTC", "ts"), L(250, 100, 335, 100, "vin"), dot(290, 100)]
    p += [optional(L(290, 100, 290, 150, "vin"), '<polygon class="part" points="278,170 302,170 290,150"/>',
                   '<polyline class="w" points="273,145 278,150 302,150 307,155"/>', L(290, 170, 290, 190),
                   gnd(290, 190), T(312, 160, "D2", "tr", "start"), T(312, 176, "SMBJ16A", "ts", "start"),
                   T(312, 192, "optional", "to", "start"))]
    p += ['<polygon class="part" points="335,88 335,112 357,100"/>',
          '<polyline class="w" points="352,84 352,88 357,88 357,112 362,112 362,116"/>',
          L(357, 100, 410, 100, "vin"), T(348, 72, "D1", "tr"), T(348, 136, "SS34", "ts")]
    p += [optional('<path class="vin" d="M410,100 a10,10 0 0 1 20,0 a10,10 0 0 1 20,0 a10,10 0 0 1 20,0 a10,10 0 0 1 20,0"/>',
                   T(450, 76, "L1", "tr"), T(450, 124, "10 µH, 3 A", "ts"), T(450, 139, "optional", "to"))]
    p += [L(490, 100, 680, 100, "vin"), dot(540, 100), dot(635, 100)]
    p += [optional(ecap(540, "C1", "100 µF 25 V", "vin"), ccap(635, "C2", "10 µF 25 V", "vin"),
                   T(588, 248, "optional, with L1", "to"))]
    p += ['<rect class="box" x="680" y="70" width="130" height="80" rx="4"/>', T(745, 92, "U1", "tr"),
          T(745, 111, "TSR 3-2450", "ts"), T(745, 127, "5 V · 3 A", "ts"), T(672, 93, "1", "tp"),
          T(818, 93, "3", "tp"), T(753, 168, "2", "tp", "start"), L(745, 150, 745, 180), gnd(745, 180)]
    p += [L(810, 100, 950, 100, "pwr"), dot(850, 100, "dotp"), dot(925, 100, "dotp"),
          ecap(850, "C3", "100 µF 10 V", "pwr"),
          optional(ccap(925, "C4", "10 µF", "pwr"), T(925, 248, "optional", "to")),
          T(954, 105, "+5V", "tn", "start")]
    return sheet(1000, 262, "Power input schematic", p)


def links():
    q = ['<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
         'orient="auto-start-reverse"><path class="arh" d="M0,0 L10,5 L0,10 z"/></marker></defs>']
    # The six link wires run straight, pin to pin; which end drives each is the firmware's choice.
    # Drawn as SPI with P4-A the master: the arrow points from the pin's driver.
    link = [(200, "G8 SCLK", "A"), (225, "G9 MOSI", "A"), (250, "G10 MISO", "B"),
            (275, "G11 CS", "A"), (300, "G12 READY", "B"), (325, "G13 spare", "")]
    rows = [(130, "5V"), (160, "GND")] + [(y, s) for y, s, _ in link]
    q += ['<rect class="box" x="40" y="40" width="200" height="370" rx="6"/>', T(140, 68, "P4-A", "tb"),
          T(140, 88, "Unit PoE-P4, primary", "ts"), T(140, 104, "SPI master; Grove", "ts")]
    for y, s in rows + [(360, "G53 TX (Grove)"), (385, "G54 RX (Grove)")]:
        q.append(T(232, y + 4, s, "tp", "end"))
    q += ['<rect class="box" x="760" y="40" width="200" height="310" rx="6"/>', T(860, 68, "P4-B", "tb"),
          T(860, 88, "Unit PoE-P4", "ts"), T(860, 104, "SPI slave", "ts")]
    for y, s in rows:
        q.append(T(768, y + 4, s, "tp", "start"))
    q += [L(240, 130, 280, 130, "pwr"), T(286, 134, "+5V", "tn", "start"),
          L(720, 130, 760, 130, "pwr"), T(714, 134, "+5V", "tn", "end"),
          L(240, 160, 270, 160), L(270, 160, 270, 170), gnd(270, 170),
          L(760, 160, 730, 160), L(730, 160, 730, 170), gnd(730, 170)]

    def wire(pts, cls, arrow=True):
        s = " ".join(f"{x},{y}" for x, y in pts)
        m = ' marker-end="url(#ar)"' if arrow else ""
        return f'<polyline class="{cls}" points="{s}"{m}/>'

    def series_r(x, y, ref):
        """An optional 33 ohm series resistor on a link wire, at its driving end."""
        return optional(f'<rect class="part" x="{x - 15}" y="{y - 6}" width="30" height="12" rx="2"/>',
                        T(x + 20, y - 4, ref, "ts", "start"))

    for y, _, by in link:
        if by == "A":
            q.append(wire([(240, y), (758, y)], "sig"))
        elif by == "B":
            q.append(wire([(760, y), (242, y)], "sig"))
        else:
            q.append(wire([(240, y), (760, y)], "sig", arrow=False))
    q += [series_r(310, 200, "R1"), series_r(310, 225, "R2"), series_r(690, 250, "R3"),
          T(500, 188, "straight, pin to pin; R1–R3 33 Ω, optional", "ts")]
    q += ['<rect class="box" x="380" y="440" width="280" height="88" rx="6"/>',
          T(520, 502, "AtomS3 Lite (sidekick)", "tb"), T(520, 518, "Grove cable to P4-A", "ts")]
    for x, a, b in [(410, "5V", ""), (450, "GND", ""), (540, "G1", "TX"), (610, "G2", "RX")]:
        q.append(T(x, 458, a, "tp"))
        if b:
            q.append(T(x, 471, b, "tp"))
    q += [L(410, 440, 410, 422, "pwr"), T(410, 415, "+5V", "tn"), L(450, 440, 450, 422), T(450, 415, "GND", "tg"),
          wire([(240, 360), (610, 360), (610, 438)], "grove"),
          wire([(540, 440), (540, 385), (242, 385)], "grove")]
    return sheet(1000, 545, "Board links schematic", q)


if __name__ == "__main__":
    for name, svg in (("power.svg", power()), ("links.svg", links())):
        with open(os.path.join(HERE, name), "w", encoding="utf-8") as f:
            f.write(svg)
        print(os.path.join(HERE, name))
