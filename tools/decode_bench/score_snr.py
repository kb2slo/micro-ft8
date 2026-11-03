#!/usr/bin/env python3
"""Our SNR against jt9's on an IQ/audio corpus, per band and receive path.

  python3 tools/decode_bench/score_snr.py BAND_DIR... [--jobs N] [--work DIR]

BAND_DIR is a band of a qmx_record.py corpus (its WAVs and manifest.jsonl; for example a band of
the corpus-2026-11-02 release). Each slot is converted as the firmware takes it: an IQ slot through
components/iq_frontend at 13,500 Hz, at -20 dBFS and also at its own level ("iq_raw", the device's
case: no normalisation); an audio slot with --real at -20 dBFS. jt9 and decode_recall then decode the
same file, and our SNR minus jt9's is taken over the messages both decoded, as decode.md's SNR check
does. Prints, per band and in all, the count, the mean and sd of the difference, and the share
within 3 dB. Needs tools/iq_convert and tools/decode_recall built, and jt9 on the PATH.
"""
import argparse
import json
import os
import statistics as st
import subprocess
import sys
import tempfile
from multiprocessing.pool import ThreadPool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score_ab import CONV, OURS, decode  # noqa: E402

ARMS = {
    "iq": ["--center-hz", "13500", "--normalize-db", "-20"],
    "iq_raw": ["--center-hz", "13500"],
    "audio": ["--real", "--normalize-db", "-20"],
}


def score_slot(job):
    band_dir, rec, work = job
    src = os.path.join(band_dir, rec["file"])
    band = os.path.basename(os.path.normpath(band_dir))
    arms = ("iq", "iq_raw") if rec["mode_tag"] == "iq" else ("audio",)
    res = []
    for arm in arms:
        out = os.path.join(work, f"{band}_{rec['file'][:-4]}_{arm}.wav")
        subprocess.run([CONV, *ARMS[arm], "-o", out, src], capture_output=True, check=True)
        with tempfile.TemporaryDirectory() as tmp:  # jt9 writes its own files into its cwd
            jt = decode(["jt9", "-8", "-d", "3", out], tmp)
            ours = decode([OURS, out], tmp)
        os.remove(out)
        res.append((band, arm, [ours[t][0] - jt[t][0] for t in ours if t in jt]))
    return res


def summary(d):
    within = sum(1 for x in d if abs(x) <= 3) / len(d)
    return f"n {len(d):5d}  ours - jt9 mean {st.mean(d):+.2f} dB, sd {st.stdev(d):.2f}, within 3 dB {100 * within:.0f}%"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("band_dir", nargs="+")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--work", help="directory for the conversions (default: a temporary one)")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        work = os.path.abspath(a.work or tmp)
        os.makedirs(work, exist_ok=True)
        jobs = [(os.path.abspath(b), json.loads(line), work)
                for b in a.band_dir for line in open(os.path.join(b, "manifest.jsonl")) if line.strip()]
        with ThreadPool(a.jobs) as pool:  # each job is subprocesses: threads are enough
            rows = [r for rs in pool.map(score_slot, jobs) for r in rs]
    for band in sorted({r[0] for r in rows}) + ["all"]:
        for arm in ARMS:
            d = [x for r in rows if r[1] == arm and band in ("all", r[0]) for x in r[2]]
            if len(d) > 1:
                print(f"{band:4s} {arm:7s} {summary(d)}")


if __name__ == "__main__":
    main()
