#!/usr/bin/env python3
"""Decode one slot in N of a tools/hl2_record.py recording with jt9, one JSON line per slot.

  python3 tools/decode_bench/sample_jt9.py RECORDING_DIR [HOUR ...] --out DECODES.jsonl
          [--every 8] [--jobs 4] [--center-hz 1500]

RECORDING_DIR holds one directory per stream (an hour, in a long run: 1500z, 1600z, ...), each with
one directory per band holding that band's slot WAVs and manifest.jsonl, as hl2_record.py writes a
several-receiver plan. HOUR names the stream directories to sample (all of them by default). Each
sampled slot is converted as score_ab.py converts an HL2 slot (tools/iq_convert 1,500 Hz above the
NCO, Q negated, -20 dBFS) and decoded by `jt9 -8 -d 3`; a message counts once per slot. Each line:

  {"hour", "band", "utc", "lna", "clip", "temp", "decodes": [[snr_db, hz, message], ...]}

A slot already in DECODES.jsonl (same band and UTC start) is skipped, so a run can be resumed, or
extended with the hours recorded since. build_four_band_run.py turns the file into a site run.

Needs `jt9` (WSJT-X) on PATH and tools/iq_convert built (`make -C tools/iq_convert`).

The 2026-11-05 four-band run (webapp/experiments/four-band/runs/2026-11-05/) was sampled by this
script's uncommitted predecessor, with the same conversion, decoder and slots; the same call here is
  python3 tools/decode_bench/sample_jt9.py ~/hl2_recordings/2026-11-05/four-band \\
      now 1500z 1600z 1700z 1800z 1900z 2000z 2100z --out decodes.jsonl --jobs 6
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score_ab import CONV, decode  # noqa: E402


def decode_slot(job):
    rec_dir, hour, band, rec, center_hz = job
    src = os.path.join(rec_dir, hour, band, rec["file"])
    with tempfile.TemporaryDirectory() as tmp:  # jt9 writes its own files into its cwd
        out = os.path.join(tmp, "slot_12k.wav")
        subprocess.run([CONV, "--center-hz", str(center_hz), "--negate-q", "--normalize-db", "-20", "-o", out, src],
                       capture_output=True, check=True)
        msgs = decode(["jt9", "-8", "-d", "3", out], tmp)
    return {"hour": hour, "band": band, "utc": rec["nominal_utc_start"], "lna": rec["lna_db_end"],
            "clip": rec["clipped_status_words"], "temp": rec["temp_c_max"],
            "decodes": [[snr, hz, text] for text, (snr, hz) in msgs.items()]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording_dir")
    ap.add_argument("hours", nargs="*", help="stream directories to sample (default: all)")
    ap.add_argument("--out", required=True, help="JSON lines file, appended to")
    ap.add_argument("--every", type=int, default=8, help="sample one slot in this many, per band")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--center-hz", type=float, default=1500, help="the HL2 channel above the NCO")
    a = ap.parse_args()

    if not os.access(CONV, os.X_OK):
        sys.exit(f"{CONV} is not built: make -C tools/iq_convert")
    hours = a.hours or sorted(d for d in os.listdir(a.recording_dir)
                              if os.path.isdir(os.path.join(a.recording_dir, d)))
    done = set()
    if os.path.exists(a.out):
        with open(a.out) as f:
            for line in f:
                d = json.loads(line)
                done.add((d["band"], d["utc"]))
    jobs = []
    for hour in hours:
        hour_dir = os.path.join(a.recording_dir, hour)
        for band in sorted(os.listdir(hour_dir)):
            manifest = os.path.join(hour_dir, band, "manifest.jsonl")
            if not os.path.isfile(manifest):
                continue
            with open(manifest) as f:
                recs = [json.loads(line) for line in f]
            for i, rec in enumerate(recs):
                if i % a.every == 0 and (band, rec["nominal_utc_start"]) not in done:
                    jobs.append((a.recording_dir, hour, band, rec, a.center_hz))
    print(f"{len(jobs)} slots to decode", flush=True)
    with Pool(a.jobs) as pool, open(a.out, "a") as f:
        for n, row in enumerate(pool.imap_unordered(decode_slot, jobs), 1):
            f.write(json.dumps(row) + "\n")
            f.flush()
            if n % 100 == 0:
                print(f"{n}/{len(jobs)}", flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    main()
