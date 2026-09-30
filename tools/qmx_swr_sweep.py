#!/usr/bin/env python3
"""QMX SWR sweep across the amateur bands: the bench prototype of the page's sweep (docs/architecture/radio.md).

Runs on a computer with the QMX's USB-C plugged straight in. It TRANSMITS (short carriers,
at the radio's own Tune %, 25% or more reads true -- architecture/radio.md), so pass --transmit.

  python3 tools/qmx_swr_sweep.py --port /dev/cu.usbmodemXXXX --transmit --csv sweep.csv \\
      --expect-low 40m,20m,15m,10m --note 'EFHW 40m, tune=25%'
  python3 tools/qmx_swr_plot.py sweep.csv -o sweep.svg

Per point (sequence proved on the bench, architecture/radio.md): RX;, MD6;, FA<hz>;, MD8; (keys
by itself), discard 300 ms, median of a few SW; reads. The radio refuses FA outside the bands it
is configured for; a band whose edges are both refused is skipped, so only bands this radio has
are swept. The mode and frequency it found are restored on the way out, including on Ctrl-C.
Band edges below are the common ITU/US allocations; your licence and sub-band rules are yours.
"""
import argparse
import re
import statistics
import sys
import time

import serial

# name, lo kHz, hi kHz, step kHz (chosen for ~11-20 points per band)
BANDS = [
    ("160m", 1800, 2000, 20),
    ("80m", 3500, 4000, 25),
    ("40m", 7000, 7300, 20),
    ("30m", 10100, 10150, 10),
    ("20m", 14000, 14350, 25),
    ("17m", 18068, 18168, 10),
    ("15m", 21000, 21450, 30),
    ("12m", 24890, 24990, 10),
    ("10m", 28000, 29700, 100),
    ("6m", 50000, 54000, 250),
]
SET_WAIT_S = 0.1        # setters reply nothing; the next query answers within a few ms
DISCARD_S = 0.3         # first read after keying can spike
LOW_SWR = 2.0
LOW_FRACTION = 0.75     # an --expect-low band passes when this share of its points reads <= 2

carrier_s = 0.0
keyed_at = None
cap_s = 240.0


def xfer(ser, cmd, wait=0.5):
    """Send one command; return the reply text with terminal display codes stripped."""
    ser.reset_input_buffer()
    t0 = time.monotonic()
    ser.write(cmd.encode())
    buf = b""
    while time.monotonic() - t0 < wait:
        buf += ser.read(ser.in_waiting or 1)
        if buf.endswith(b";"):
            break
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf.decode(errors="replace"))


def set_fa(ser, hz):
    xfer(ser, "MD6;", SET_WAIT_S)
    xfer(ser, f"FA{hz:011d};", SET_WAIT_S)
    return xfer(ser, "FA;") == f"FA{hz:011d};"


def rx(ser):
    global carrier_s, keyed_at
    xfer(ser, "RX;", SET_WAIT_S)
    if keyed_at is not None:
        carrier_s += time.monotonic() - keyed_at
        keyed_at = None


def measure(ser, hz, reads):
    """Key at hz via MD8; return (median SWR or None, reads, FA held while keyed)."""
    global keyed_at
    if carrier_s >= cap_s:
        raise RuntimeError("carrier cap reached")
    keyed_at = time.monotonic()
    xfer(ser, "MD8;", SET_WAIT_S)
    time.sleep(DISCARD_S)
    vals = []
    for _ in range(reads):
        m = re.search(r"SW(\d+);", xfer(ser, "SW;", 0.3))
        if m:
            vals.append(int(m.group(1)) / 100.0)
        time.sleep(0.05)
    held = xfer(ser, "FA;") == f"FA{hz:011d};"
    return (statistics.median(vals) if vals else None), vals, held


def plan(name, lo, hi, step, density):
    st = max(1, int(round(step / density)))
    pts = list(range(lo, hi + 1, st))
    if pts[-1] != hi:
        pts.append(hi)
    return [p * 1000 for p in pts]


def summarize(rows, expect_low):
    print("\nPer band (SWR, median of the reads at each point):")
    print(f"  {'band':<5} {'pts':>3} {'min':>5} {'@ MHz':>8} {'max':>5} {'<=2.0':>6}  verdict")
    ok = True
    for name in [b[0] for b in BANDS]:
        r = [(hz, m) for n, hz, m in rows if n == name and m is not None]
        if not r:
            continue
        lo = min(r, key=lambda x: x[1])
        hi = max(r, key=lambda x: x[1])
        frac = sum(1 for _, m in r if m <= LOW_SWR) / len(r)
        verdict = ""
        if expect_low is not None:
            if name in expect_low:
                good = frac >= LOW_FRACTION
                verdict = f"expected low: {'PASS' if good else 'FAIL'} (need >= {LOW_FRACTION:.0%} of points <= 2)"
            else:
                good = lo[1] > LOW_SWR
                verdict = f"expected high: {'PASS' if good else 'FAIL'} (need min > 2)"
            ok &= good
        print(f"  {name:<5} {len(r):>3} {lo[1]:>5.2f} {lo[0] / 1e6:>8.3f} {hi[1]:>5.2f} {frac:>6.0%}  {verdict}")
    if expect_low is not None:
        swept = {n for n, _, m in rows if m is not None}
        for name in sorted(expect_low - swept):
            print(f"  {name:<5} NOT SWEPT: expected low, but the radio gave no readings for it")
            ok = False
        print("\nOverall:", "PASS" if ok else "FAIL")


def main():
    global cap_s
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--transmit", action="store_true", help="required: this keys the radio at its Tune power")
    ap.add_argument("--bands", default="all", help="comma list, e.g. 40m,20m (default: every band the radio accepts)")
    ap.add_argument("--density", type=float, default=1.0, help="points multiplier; 2 halves every step")
    ap.add_argument("--reads", type=int, default=3, help="SW; reads per point (median)")
    ap.add_argument("--cap", type=float, default=cap_s, help="total keyed-time cap, seconds")
    ap.add_argument("--expect-low", default=None, help="bands expected to read <= 2, e.g. 40m,20m,15m,10m; others expected > 2")
    ap.add_argument("--csv", default="", help="write band,freq_hz,swr_median,reads,fa_held for qmx_swr_plot.py")
    ap.add_argument("--note", default="", help="free text echoed first, e.g. 'EFHW 40m, tune=25%%'")
    args = ap.parse_args()
    if not args.transmit:
        sys.exit("refusing to transmit without --transmit")
    cap_s = args.cap
    names = {b[0] for b in BANDS}
    want = names if args.bands == "all" else set(args.bands.split(","))
    if want - names:
        sys.exit(f"unknown band(s): {sorted(want - names)}; known: {sorted(names)}")
    expect_low = None if args.expect_low is None else set(args.expect_low.split(","))
    if args.note:
        print(f"Note: {args.note}")

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    time.sleep(0.3)
    vn, orig_fa, orig_md = xfer(ser, "VN;"), xfer(ser, "FA;"), xfer(ser, "MD;")
    print(f"Radio: {vn}  (was {orig_fa} {orig_md})")
    if not (orig_fa.startswith("FA") and orig_md.startswith("MD")):
        sys.exit("could not read the original FA;/MD;; not transmitting")

    rows = []  # (band, hz, median)
    csv_rows = []
    try:
        for name, lo, hi, step in BANDS:
            if name not in want:
                continue
            rx(ser)
            if not set_fa(ser, lo * 1000) and not set_fa(ser, hi * 1000):
                print(f"{name:>5}: radio refuses both edges; band not configured, skipped")
                continue
            pts = plan(name, lo, hi, step, args.density)
            print(f"{name:>5}: {len(pts)} points, {pts[0] / 1e6:.3f}-{pts[-1] / 1e6:.3f} MHz")
            for hz in pts:
                rx(ser)
                if not set_fa(ser, hz):
                    print(f"       {hz / 1e6:9.3f} MHz  FA refused; skipped")
                    continue
                m, vals, held = measure(ser, hz, args.reads)
                rows.append((name, hz, m))
                csv_rows.append((name, hz, m, vals, held))
                flag = "" if held else "  FA MOVED WHILE KEYED"
                print(f"       {hz / 1e6:9.3f} MHz  {'--' if m is None else f'{m:.2f}'}{flag}")
    except (KeyboardInterrupt, RuntimeError) as e:
        print(f"\nStopped early: {e or 'Ctrl-C'}")
    finally:
        for _ in range(2):
            xfer(ser, "RX;", SET_WAIT_S)
        xfer(ser, orig_md if orig_md.startswith("MD") else "MD6;", SET_WAIT_S)
        xfer(ser, "FA" + orig_fa[2:].rstrip(";") + ";", SET_WAIT_S)
        now_fa, now_md = xfer(ser, "FA;"), xfer(ser, "MD;")
        print(f"\nRestored: {now_fa} {now_md}" + ("" if (now_fa, now_md) == (orig_fa, orig_md) else "   <-- DIFFERS from the start"))
        print(f"Keyed time: {carrier_s:.1f} s (cap {cap_s:.0f} s)")
        ser.close()

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("band,freq_hz,swr_median,reads,fa_held\n")
            for n, hz, m, vals, held in csv_rows:
                f.write(f"{n},{hz},{'' if m is None else m},{' '.join(map(str, vals))},{int(held)}\n")
        print(f"wrote {args.csv}")
    summarize(rows, expect_low)


if __name__ == "__main__":
    main()
