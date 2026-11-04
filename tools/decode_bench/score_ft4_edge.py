#!/usr/bin/env python3
"""FT4 in a QMX I/Q recording: jt9 -5 on each 7.5 s period, decodes binned by audio offset.

The 2026-11-02 QMX corpus (tools/decode_bench/corpus/qmx-2026-11-02) was recorded with the dial on
FT8's, so on 20 m FT4 (14.080 MHz) sits 19.5 kHz above the LO, near the top of the radio's 48 kHz
window. This decodes it there, to see whether it decodes at all and whether decodes thin out toward
the window's edge (row an8x, the receive dial; docs/roadmap/several-channels.md, Open).

  python3 tools/decode_bench/score_ft4_edge.py BAND_DIR [--center-hz 19500]

Each I/Q slot in BAND_DIR's manifest.jsonl goes through tools/iq_convert at --center-hz, 12 kHz out,
then is cut into its two 7.5 s FT4 periods for `jt9 -5 -d 3`. Our own decoder is not run: it decodes
FT8 only until an8x. Needs `jt9` on PATH and tools/iq_convert built. Work files go in a temporary
directory and are removed.
"""
import argparse
import json
import os
import re
import statistics as st
import subprocess
import tempfile
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONV = os.path.join(ROOT, "tools", "iq_convert", "iq_convert")
LINE = re.compile(r"^\s*\d{4,6}\s+([-+]?\d+)\s+([-+]?\d+\.\d+)\s+(\d+)\s+\+\s+(.*?)\s*$")
PERIOD_BYTES = 90000 * 2  # 7.5 s of 16-bit mono at 12 kHz


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("band_dir")
    ap.add_argument("--center-hz", type=float, default=19500, help="the FT4 channel above the LO")
    a = ap.parse_args()
    recs = [json.loads(l) for l in open(os.path.join(a.band_dir, "manifest.jsonl"))]
    iq = [r for r in recs if r["mode_tag"] == "iq"]
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for r in iq:
            wav12 = os.path.join(tmp, "conv.wav")
            subprocess.run([CONV, "--center-hz", str(a.center_hz), "--output-rate", "12000", "--normalize-db", "-20",
                            "-o", wav12, os.path.join(a.band_dir, r["file"])], capture_output=True, check=True)
            w = wave.open(wav12)
            data = w.readframes(w.getnframes())
            w.close()
            for k in (0, 1):
                name = f"p{k}.wav"
                o = wave.open(os.path.join(tmp, name), "wb")
                o.setnchannels(1)
                o.setsampwidth(2)
                o.setframerate(12000)
                o.writeframes(data[k * PERIOD_BYTES:(k + 1) * PERIOD_BYTES])
                o.close()
                out = subprocess.run(["jt9", "-5", "-d", "3", name], cwd=tmp, capture_output=True, text=True).stdout
                for line in out.splitlines():
                    m = LINE.match(line)
                    if m:
                        rows.append((r["file"], k, int(m.group(1)), float(m.group(2)), int(m.group(3))))
    periods = 2 * len(iq)
    print(f"{len(iq)} I/Q slots, {periods} FT4 periods, {len(rows)} FT4 decodes ({len(rows) / max(periods, 1):.2f} a period), "
          f"{len({(x[0], x[1]) for x in rows})} periods with any")
    if rows:
        print(f"SNR median {st.median([x[2] for x in rows])} dB (min {min(x[2] for x in rows)}); "
              f"DT median {st.median([x[3] for x in rows])} s")
        for lo in range(0, 3000, 500):
            sel = [x for x in rows if lo <= x[4] < lo + 500]
            print(f"  audio {lo:4d}-{lo + 499:4d} Hz ({(a.center_hz - 1500 + lo) / 1000:.1f} kHz above the LO): "
                  f"{len(sel):4d}" + (f", SNR median {st.median([x[2] for x in sel])}" if sel else ""))


if __name__ == "__main__":
    main()
