#!/usr/bin/env python3
"""Key a QMX over CAT from a laptop, the way poe_p4/main/tx_scheduler.cpp does (README.md beside this).

    key_qmx.py --port <QMX serial port> --mode FT4 --dial 14074000 --base 7500 --count 6 \
        --message "CQ <CALL> <GRID>" [--tx-dial HZ --via split|retune] [--dry-run]

Per transmission: FA<dial>; TX; then one TA<base + tone * spacing>; per symbol, the first at the
slot start + 0.5 s (the P4's lead), then TA0; 5 ms RX;. With --tx-dial, the transmission goes out
at --tx-dial plus the tones instead (row xvfn): --via split puts VFO B there (SP1; FB; before TX;,
FB<dial>; SP0; FR0; FT0; after RX;), --via retune moves VFO A there and back (FA). Each set is read
back and timed before keying; a read-back that does not match skips that transmission. Transmits in alternate slots (even ones)
so a test does not hog the frequency. The laptop's clock must be NTP-synced. These are real
transmissions: the control operator is at the station. Ctrl-C sends TA0; and RX;.
"""
import argparse, os, signal, subprocess, sys, time
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
MODES = {  # slot s, symbol s, tone spacing Hz: protocol.h's kProtocolFT8 / kProtocolFT4
    "FT8": (15.0, 0.160, 6.25),
    "FT4": (7.5, 0.048, 20.8333),
}
LEAD_S = 0.5
TONES = os.path.join(HERE, "ftx_tones.exe" if os.name == "nt" else "ftx_tones")
BANDS = {"20 m": (14_000_000, 14_350_000), "40 m": (7_000_000, 7_300_000)}
SETUP_S = 1.0  # split or retune starts this long before the slot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--mode", choices=MODES, required=True)
    ap.add_argument("--dial", type=int, default=14_074_000, help="FA, Hz")
    ap.add_argument("--base", type=float, required=True, help="TA base tone, Hz above the dial")
    ap.add_argument("--message", required=True, help='e.g. "CQ <CALL> <GRID>"')
    ap.add_argument("--count", type=int, default=3, help="transmissions")
    ap.add_argument("--tx-dial", type=int, help="transmit around this dial instead of --dial, Hz")
    ap.add_argument("--via", choices=("split", "retune"), help="how --tx-dial is reached")
    ap.add_argument("--dry-run", action="store_true", help="print what would be sent; open no port")
    a = ap.parse_args()

    if (a.tx_dial is None) != (a.via is None):
        sys.exit("--tx-dial and --via go together")
    tx_dial = a.tx_dial if a.tx_dial is not None else a.dial
    slot_s, sym_s, spacing = MODES[a.mode]
    tones = [int(t) for t in subprocess.check_output(
        [TONES, a.mode, a.message], text=True).split()]
    lo = tx_dial + a.base
    hi = lo + max(tones) * spacing
    band = next((b for b, (f0, f1) in BANDS.items() if f0 <= a.dial <= f1 and f0 <= lo and hi + 50 <= f1), None)
    if band is None:
        sys.exit(f"refused: dial {a.dial}, RF {lo:.0f}-{hi:.0f} Hz is not inside one of " + ", ".join(BANDS))
    print(f"{a.mode} '{a.message}': {len(tones)} tones, RF {lo/1e6:.6f}-{hi/1e6:.6f} MHz on {band} "
          f"(dial {a.dial}" + (f", TX dial {tx_dial} by {a.via}" if a.via else "")
          + f", TA {a.base:.0f}-{a.base + max(tones) * spacing:.1f}), {a.count} transmissions")

    before, after = [], []
    if a.via == "split":
        before = ["SP1;", "FB%011d;" % tx_dial]
        after = ["FB%011d;" % a.dial, "SP0;", "FR0;", "FT0;"]
    elif a.via == "retune":
        before = ["FA%011d;" % tx_dial]
        after = ["FA%011d;" % a.dial]
    if a.dry_run:
        print(" ".join(before + ["TX;"]) + " " + " ".join("TA%.2f;" % (a.base + t * spacing) for t in tones[:4])
              + " ... TA0; RX; " + " ".join(after))
        return

    port = serial.Serial(a.port, 115200, timeout=0.2)

    def cmd(s):
        port.write(s.encode())

    def ask(q, want=None, timeout=0.5):
        """Send a query and return (reply, ms) for the first reply to it; with want, wait for that reply."""
        port.reset_input_buffer(); t0 = time.time(); cmd(q); buf = ""
        while time.time() - t0 < timeout:
            buf += port.read(64).decode(errors="replace")
            for r in buf.split(";"):
                if r.startswith(q[:2]) and (want is None or r == want):
                    return r + ";", (time.time() - t0) * 1000
        return buf.strip() or "(none)", None

    def run(cmds):
        for c in cmds:
            cmd(c); time.sleep(0.03)

    def unkey(*_):
        cmd("TA0;"); time.sleep(0.01); cmd("RX;"); time.sleep(0.03); run(after); port.flush()
        print("\nunkeyed" + (", " + " ".join(after) if after else "")); sys.exit(1)

    signal.signal(signal.SIGINT, unkey)
    cmd("FA%011d;" % a.dial); time.sleep(0.1); port.reset_input_buffer()
    cmd("FA;"); time.sleep(0.1); print("radio:", port.read(64).decode(errors="replace").strip())

    sent = 0
    while sent < a.count:
        now = time.time()
        slot = int(now // slot_s) + 1
        if slot % 2:
            slot += 1  # even slots only
        start = slot * slot_s
        if start - now < SETUP_S + 0.5:
            slot += 2; start = slot * slot_s
        if before:
            time.sleep(max(0.0, start - SETUP_S - time.time()))
            t0 = time.time(); run(before)
            if a.via == "split":
                got, ms = ask("SP;", "SP1")
            else:
                got, ms = ask("FA;", "FA%011d" % tx_dial)
            print(f"  {' '.join(before)} then read back {got} in {(time.time() - t0) * 1000:.0f} ms"
                  + ("" if ms is not None else ": no match, skipped"))
            if ms is None:
                run(after); slot += 2; continue
        time.sleep(max(0.0, start - 0.3 - time.time()))
        cmd("TX;")
        late = []
        for i, t in enumerate(tones):
            due = start + LEAD_S + i * sym_s
            d = due - time.time()
            if d > 0:
                time.sleep(d)
            late.append(time.time() - due)
            cmd("TA%.2f;" % (a.base + t * spacing))
        time.sleep(max(0.0, start + LEAD_S + len(tones) * sym_s - time.time()))
        cmd("TA0;"); time.sleep(0.005); cmd("RX;")
        if after:
            time.sleep(0.03); t0 = time.time(); run(after)
            if a.via == "split":
                got = " ".join(ask(q)[0] for q in ("SP;", "FR;", "FT;"))
            else:
                got, _ = ask("FA;", "FA%011d" % a.dial)
            print(f"  {' '.join(after)} then read back {got} in {(time.time() - t0) * 1000:.0f} ms")
        sent += 1
        print(time.strftime("%H:%M:%S", time.localtime(start)),
              f"sent {sent}/{a.count}, symbol lateness max {max(late)*1000:.1f} ms")
    port.close()


if __name__ == "__main__":
    main()
