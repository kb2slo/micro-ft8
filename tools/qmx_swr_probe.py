#!/usr/bin/env python3
"""QMX SWR-sweep CAT probe (the SWR sweep, docs/architecture/radio.md): does MD8; + FA + SW; behave as a sweep needs?

Runs on a computer with the QMX's USB-C plugged straight in (it enumerates as a serial
port). No firmware involved. It TRANSMITS, at the radio's own Tune %, so use a dummy
load first and pass --i-have-a-load to acknowledge it.

  python3 tools/qmx_swr_probe.py --port /dev/cu.usbmodemXXXX --i-have-a-load

Needs pyserial. Paste the whole output back; every reply and its timing is printed.
Commands follow the QMX CAT manual (1_04_003): VN; firmware, MD8; SWR Tune mode,
TX;/RX; key and unkey, FA<11 digits>; VFO A, SW; SWR in hundredths ("SW;" alone in
receive), QB;/QC; CAT timeout. (SR; returns ?; on 1_04_003.)
"""
import argparse
import re
import sys
import time

import serial

BANDS = {"20m": [14000000, 14075000, 14150000, 14225000, 14350000]}

MAX_CARRIER_TOTAL_S = 30.0  # hard cap on keyed time for the whole run
SETTLE_POLLS = 10           # SW; polls after each step
SETTLE_GAP_S = 0.1
SET_WAIT_S = 0.1            # setters reply nothing; do not wait long for one
SETTLE_DISCARD_S = 0.3      # first SW; after keying has read 3-5 (transients)

carrier_s = 0.0
keyed_at = None


def xfer(ser, cmd, wait=0.5):
    """Send one command; return (reply text up to the last ';', elapsed seconds)."""
    ser.reset_input_buffer()
    t0 = time.monotonic()
    ser.write(cmd.encode())
    buf = b""
    while time.monotonic() - t0 < wait:
        buf += ser.read(ser.in_waiting or 1)
        if buf.endswith(b";"):
            break
    # The radio sometimes appends terminal display codes (ESC[7m ...) after a reply.
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf.decode(errors="replace"))
    return text, time.monotonic() - t0


def show(ser, cmd, wait=0.5):
    reply, dt = xfer(ser, cmd, wait)
    print(f"  {cmd:<14} -> {reply or '(no reply)':<28} {dt * 1000:6.0f} ms")
    return reply


def start_carrier():
    """Keyed time counts from MD8; (it may key by itself), not just TX;."""
    global carrier_s, keyed_at
    if carrier_s >= MAX_CARRIER_TOTAL_S:
        raise RuntimeError("carrier cap reached")
    keyed_at = time.monotonic()


def stop_carrier(ser):
    global carrier_s, keyed_at
    show(ser, "RX;", SET_WAIT_S)
    if keyed_at is not None:
        carrier_s += time.monotonic() - keyed_at
        keyed_at = None


def poll_sw(ser, label):
    print(f"  {label}: SW; x{SETTLE_POLLS}, {SETTLE_GAP_S * 1000:.0f} ms apart")
    t0 = time.monotonic()
    for _ in range(SETTLE_POLLS):
        reply, dt = xfer(ser, "SW;", 0.3)
        print(f"    t+{(time.monotonic() - t0) * 1000:5.0f} ms  {reply or '(no reply)'}")
        time.sleep(SETTLE_GAP_S)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--band", default="20m", choices=sorted(BANDS))
    ap.add_argument("--i-have-a-load", action="store_true",
                    help="required: this transmits at the radio's Tune power")
    ap.add_argument("--note", default="", help="free text echoed first, e.g. 'load=dummy tune=25%%'")
    ap.add_argument("--read-only", action="store_true", help="step 1 only; never keys")
    args = ap.parse_args()
    if not args.read_only and not args.i_have_a_load:
        sys.exit("refusing to transmit without --i-have-a-load (or use --read-only)")

    if args.note:
        print(f"Note: {args.note}")
    ser = serial.Serial(args.port, 115200, timeout=0.05)
    time.sleep(0.3)

    print("Step 1: read-only queries")
    for c in ("VN;", "OM;", "ID;", "FA;", "MD;", "IF;", "QB;", "QC;", "SW;"):
        show(ser, c)
    orig_fa = xfer(ser, "FA;")[0]
    orig_md = xfer(ser, "MD;")[0]
    if args.read_only:
        return
    if not (orig_fa.startswith("FA") and orig_md.startswith("MD")):
        sys.exit("could not read the original FA;/MD;; not transmitting")

    try:
        print("\nStep 2: does MD8; alone key? (no TX; sent)")
        start_carrier()
        show(ser, "MD8;", SET_WAIT_S)
        for c in ("MD;", "KD;", "TQ;", "IF;", "SW;"):
            show(ser, c)
        poll_sw(ser, "after MD8; alone")
        stop_carrier(ser)
        for c in ("MD;", "KD;", "SW;"):
            show(ser, c)

        pts = BANDS[args.band]
        print(f"\nStep 3: per point RX; FA; verify; MD8; discard settle; poll ({args.band}); first point repeated last")
        for hz in pts + pts[:1]:
            stop_carrier(ser)
            show(ser, "MD6;", SET_WAIT_S)  # v2 saw MD8; keep its old frequency when already in SWR Tune
            show(ser, f"FA{hz:011d};", SET_WAIT_S)
            show(ser, "FA;")
            start_carrier()
            show(ser, "MD8;", SET_WAIT_S)
            time.sleep(SETTLE_DISCARD_S)
            show(ser, "KD;")
            show(ser, "FA;")  # still the requested frequency while keyed?
            poll_sw(ser, f"{hz} Hz (after {SETTLE_DISCARD_S * 1000:.0f} ms discard)")
        stop_carrier(ser)

        hz = pts[2]
        print(f"\nStep 3x: plain TX; (no MD8;) at {hz} Hz after a sweep, in whatever mode the radio is left in")
        show(ser, "MD;")
        show(ser, f"FA{hz:011d};", SET_WAIT_S)
        show(ser, "FA;")
        start_carrier()
        show(ser, "TX;", SET_WAIT_S)
        time.sleep(SETTLE_DISCARD_S)
        show(ser, "KD;")
        show(ser, "FA;")
        poll_sw(ser, f"{hz} Hz, TX; only")
        stop_carrier(ser)
    finally:
        print("\nStep 4: restore")
        for _ in range(2):  # belt and braces: unkey twice
            xfer(ser, "RX;")
        show(ser, orig_md if orig_md.startswith("MD") else "MD6;", SET_WAIT_S)
        if orig_fa.startswith("FA"):
            show(ser, "FA" + orig_fa[2:].rstrip(";") + ";", SET_WAIT_S)
        show(ser, "MD;")
        show(ser, "FA;")
        print(f"\nKeyed time this run: {carrier_s:.1f} s (cap {MAX_CARRIER_TOTAL_S:.0f} s)")
        print(f"Original: {orig_fa} {orig_md}")
        ser.close()


if __name__ == "__main__":
    main()
