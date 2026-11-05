#!/usr/bin/env python3
"""Score a `tools/qmx_record.py --alternate` run: the IQ arm against the audio arm.

Each slot in BAND_DIR's manifest.jsonl is converted to 12 kHz with tools/iq_convert (IQ slots
through the front end, audio slots with --real) and decoded by `jt9 -8 -d 3` and by
tools/decode_recall at the firmware's settings and at t4/f2. It prints, per arm, decodes per
slot and our share of jt9's; the paired difference of each IQ turn against the audio turn after
it; and image rejection, from each strong station's SNR minus its SNR as a ghost on the far side
of the LO (the IQ slot converted with --swap-iq).

  python3 tools/decode_bench/score_ab.py BAND_DIR [--conv-dir conv] [--iq-flag=--balance] [--jobs 4]

With --arm-field FIELD the arms are that manifest field's values instead of IQ and audio, in the
order they first appear, every slot converted by its own mode_tag: an HL2 run with
tools/hl2_record.py --lna-ab scores as --arm-field lna_arm_db, the first arm's turns paired against
the second's.

Needs `jt9` (WSJT-X) on PATH and tools/iq_convert and tools/decode_recall built (`make -C`).
Conversions go in BAND_DIR/CONV_DIR, so a run with other --iq-flag options keeps its own.
How the 2026-11-02 corpus was scored with it: tools/decode_bench/corpus/qmx-2026-11-02/README.md.
"""
import argparse
import json
import os
import re
import statistics as st
import subprocess
import tempfile
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONV = os.path.join(ROOT, "tools", "iq_convert", "iq_convert")
OURS = os.path.join(ROOT, "tools", "decode_recall", "decode_recall")
LINE = re.compile(r"^\s*\d{4,6}\s+([-+]?\d+)\s+[-+]?\d+\.\d+\s+(\d+)\s+~\s+(.*?)\s*$")


def decode(cmd, cwd):
    """{message: (snr, hz)} from one decoder's WSJT-X-style lines."""
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd).stdout
    msgs = {}
    for line in out.splitlines():
        m = LINE.match(line)
        if m:
            text = re.sub(r"\s+a\d$", "", m.group(3)).strip()  # jt9's AP-pass marker
            msgs.setdefault(text, (int(m.group(1)), int(m.group(2))))
    return msgs


def score_slot(job):
    band_dir, rec, a = job
    src = os.path.join(band_dir, rec["file"])
    conv = os.path.join(band_dir, a.conv_dir)
    os.makedirs(conv, exist_ok=True)
    base = rec["file"][:-4]
    out = os.path.join(conv, base + "_12k.wav")
    level = ["--normalize-db", str(a.normalize_db)]
    if rec["mode_tag"] == "iq":
        iq = ["--center-hz", str(a.center_hz)] + a.iq_flag
        subprocess.run([CONV, *iq, *level, "-o", out, src], capture_output=True, check=True)
    else:
        subprocess.run([CONV, "--real", *level, "-o", out, src], capture_output=True, check=True)
    with tempfile.TemporaryDirectory() as tmp:  # jt9 writes its own files into its cwd
        jt = decode(["jt9", "-8", "-d", "3", out], tmp)
        ours = decode([OURS, out], tmp)
        ours4 = decode([OURS, "--time-osr", "4", out], tmp)
        image = None
        if rec["mode_tag"] == "iq":
            wrong = os.path.join(conv, base + "_image_12k.wav")
            subprocess.run([CONV, "--center-hz", str(a.center_hz), "--swap-iq", *a.iq_flag, *level,
                            "-o", wrong, src], capture_output=True, check=True)
            ghosts = decode(["jt9", "-8", "-d", "3", wrong], tmp)
            image = [jt[t][0] - s for t, (s, _) in ghosts.items() if t in jt]
    return rec["file"], rec["mode_tag"], jt, ours, ours4, image


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("band_dir")
    ap.add_argument("--conv-dir", default="conv", help="subdirectory for the 12 kHz conversions")
    ap.add_argument("--center-hz", type=float, default=13500, help="IQ channel centre above the LO")
    ap.add_argument("--normalize-db", type=float, default=-20, help="output RMS, dBFS, both arms")
    ap.add_argument("--iq-flag", action="append", default=[], help="extra iq_convert flag for IQ slots (repeatable)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--arm-field", help="split arms by this manifest field instead of IQ against audio")
    a = ap.parse_args()
    recs = [json.loads(l) for l in open(os.path.join(a.band_dir, "manifest.jsonl"))]
    with Pool(a.jobs) as p:
        res = sorted(p.map(score_slot, [(a.band_dir, r, a) for r in recs]))

    print(f"{a.band_dir}: {len(res)} slots, dial {recs[0].get('dial_hz')}, "
          f"overflows {sum(r['overflows'] for r in recs)}, iq flags {a.iq_flag or 'none'}")
    arm_of = {r["file"]: (str(r.get(a.arm_field)) if a.arm_field else r["mode_tag"]) for r in recs}
    arms = list(dict.fromkeys(arm_of[r["file"]] for r in recs)) if a.arm_field else ["iq", "audio"]
    images = []
    for tag in arms:
        rows = [r for r in res if arm_of[r[0]] == tag]
        if not rows:
            continue
        jt = [len(r[2]) for r in rows]
        ours = [len(r[3]) for r in rows]
        hit = sum(len(set(r[3]) & set(r[2])) for r in rows)
        hit4 = sum(len(set(r[4]) & set(r[2])) for r in rows)
        snrs = [s for r in rows for s, _ in r[2].values()]
        print(f"  {(a.arm_field + '=' if a.arm_field else '') + tag:5s} {len(rows):3d} slots  jt9 {st.mean(jt):5.1f}/slot (median {st.median(jt):4.1f})  "
              f"ours {st.mean(ours):5.1f}/slot ({100 * hit / max(1, sum(jt)):5.1f}% of jt9)  "
              f"t4/f2 {st.mean(len(r[4]) for r in rows):5.1f}/slot ({100 * hit4 / max(1, sum(jt)):5.1f}% of jt9)  "
              f"jt9 SNR median {st.median(snrs) if snrs else float('nan'):+.0f} dB")
        images += [d for r in rows for d in (r[5] or [])]

    # Turns: runs of consecutive slots in one arm. Each first-arm turn against the second-arm turn after it.
    first, second = (arms + [None, None])[:2]
    turns = []
    for r in res:
        if turns and arm_of[turns[-1][0][0]] == arm_of[r[0]]:
            turns[-1].append(r)
        else:
            turns.append([r])
    dj, do = [], []
    for t1, t2 in zip(turns, turns[1:]):
        if arm_of[t1[0][0]] == first and arm_of[t2[0][0]] == second:
            dj.append(sum(len(r[2]) for r in t1) - sum(len(r[2]) for r in t2))
            do.append(sum(len(r[3]) for r in t1) - sum(len(r[3]) for r in t2))
    if len(dj) > 1:
        se = lambda d: st.stdev(d) / len(d) ** 0.5
        label = f"{first}−{second}" if a.arm_field else "IQ−audio"
        print(f"  paired {label} per turn ({len(dj)} pairs): jt9 {st.mean(dj):+.2f} ± {se(dj):.2f}, "
              f"ours {st.mean(do):+.2f} ± {se(do):.2f}  (± one standard error)")
    if images:
        print(f"  image rejection from {len(images)} wrong-side decodes: median {st.median(images):.0f} dB, "
              f"range {min(images)}–{max(images)} dB")


if __name__ == "__main__":
    main()
