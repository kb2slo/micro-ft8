#!/usr/bin/env python3
"""Score a no-key retune run: do decodes thin out after the dial moves away and back? (docs/architecture/radio.md, TX timing on a QMX)

    python3 tools/qmx_tx_probe/score_retune.py REC_DIR PROBE_LOG [--ft8-center-hz 13500] [--ft4-center-hz -13000]

REC_DIR is a tools/qmx_record.py recording in I/Q mode, made beside
`key_qmx.py --via retune --no-key --every 8` on FT4, which moves the dial at the start of each minute: to
FT4's dial a second before it, and back where an FT4 transmission would have ended, about 5.5 s in.
PROBE_LOG is that run's output, for which minutes moved.

Each slot is converted by tools/iq_convert at FT8's and FT4's offsets from the LO (40 m with the dial on
FT8's: +13,500 and -13,000 Hz), and `jt9` decodes FT8 over the slot and FT4 over each 7.5 s half. In a
minute that moved, by second of the minute:

* :00 is the moved slot: FT8 not counted (an FT4 transmission makes us deaf to it), its first FT4 half not
  counted, its second FT4 half is **FT4 after** (it starts about 2 s after the dial is back).
* :15 is **FT8 after**; :45 is **FT8 control**, the same parity (40 m's two parities carry different
  stations, so :30 is no control for :15). Its move out, a second before the next minute, falls after
  its signals end.
* :30 is even, as :00 is: its second half is **FT4 control**, and its FT8 is reported beside :00's.

Needs `jt9` on PATH and tools/iq_convert built (`make -C tools/iq_convert`).
"""
import argparse, os, re, statistics as st, subprocess, tempfile, wave

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONV = os.path.join(ROOT, "tools", "iq_convert", "iq_convert")
DECODE = re.compile(r"^\s*\d{4,6}\s+-?\d+\s+-?\d+\.\d\s+\d+\s+[~+]\s+(.+?)\s*$")
HALF = 90000 * 2  # 7.5 s of 16-bit mono at 12 kHz


def jt9(mode, path, cwd):
    out = subprocess.run(["jt9", mode, "-d", "3", path], cwd=cwd, capture_output=True, text=True).stdout
    return {m.group(1) for m in map(DECODE.match, out.splitlines()) if m}


def convert(center, src, dst):
    subprocess.run([CONV, "--center-hz", str(center), "--output-rate", "12000", "--normalize-db", "-20",
                    "-o", dst, src], capture_output=True, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rec_dir")
    ap.add_argument("probe_log")
    ap.add_argument("--ft8-center-hz", type=float, default=13500)
    ap.add_argument("--ft4-center-hz", type=float, default=-13000)
    a = ap.parse_args()
    moved = {l[3:5] for l in open(a.probe_log) if re.match(r"\d\d:\d\d:00 moved", l)}  # minutes, any zone
    files = sorted(f for f in os.listdir(a.rec_dir) if re.fullmatch(r"\d{6}_\d{6}\.wav", f))
    rows = {}  # (minute, second) -> (ft8 count, [ft4 half 0, half 1])
    with tempfile.TemporaryDirectory() as tmp:
        for f in files:
            mm, ss = f[9:11], f[11:13]
            src = os.path.join(a.rec_dir, f)
            convert(a.ft8_center_hz, src, os.path.join(tmp, "ft8.wav"))
            n8 = len(jt9("-8", "ft8.wav", tmp))
            convert(a.ft4_center_hz, src, os.path.join(tmp, "ft4.wav"))
            w = wave.open(os.path.join(tmp, "ft4.wav")); data = w.readframes(w.getnframes()); w.close()
            n4 = []
            for k in (0, 1):
                o = wave.open(os.path.join(tmp, f"h{k}.wav"), "wb")
                o.setnchannels(1); o.setsampwidth(2); o.setframerate(12000)
                o.writeframes(data[k * HALF:(k + 1) * HALF]); o.close()
                n4.append(len(jt9("-5", f"h{k}.wav", tmp)))
            rows[(mm, ss)] = (n8, n4)
            print(f"{f}  {'moved ' if mm in moved else 'idle  '}:{ss}  FT8 {n8:3d}  FT4 {n4[0]:2d} {n4[1]:2d}")
    pairs = [(mm, rows.get((mm, "15")), rows.get((mm, "30")), rows.get((mm, "00")), rows.get((mm, "45")))
             for mm in sorted(moved)]
    pairs = [p for p in pairs if all(p[1:])]
    if not pairs:
        print("no whole moved minute recorded"); return
    f8a = [p[1][0] for p in pairs]; f8c = [p[4][0] for p in pairs]
    f4a = [p[3][1][1] for p in pairs]; f4c = [p[2][1][1] for p in pairs]
    print(f"\n{len(pairs)} moved minutes, paired within each minute (after vs control):")
    print(f"  FT8 after {sum(f8a)} ({st.mean(f8a):.1f} a slot), control {sum(f8c)} ({st.mean(f8c):.1f}); "
          f"after minus control per minute: {[x - y for x, y in zip(f8a, f8c)]}")
    print(f"  FT4 after {sum(f4a)} ({st.mean(f4a):.1f} a half), control {sum(f4c)} ({st.mean(f4c):.1f}); "
          f"after minus control per minute: {[x - y for x, y in zip(f4a, f4c)]}")
    m0 = [p[3][0] for p in pairs]; m30 = [p[2][0] for p in pairs]
    print(f"  FT8 in the moved :00 slot {sum(m0)} ({st.mean(m0):.1f} a slot), even control :30 {sum(m30)} "
          f"({st.mean(m30):.1f}): what an FT4 transmission's deafness already costs")


if __name__ == "__main__":
    main()
