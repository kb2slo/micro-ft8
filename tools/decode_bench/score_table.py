#!/usr/bin/env python3
"""Per stream and band: a several-receiver tools/hl2_record.py run's score_ab.py results beside its radio counters.

  python3 tools/decode_bench/score_table.py RECORDING_DIR [HOUR ...] [--utc-offset-hours -6]

RECORDING_DIR holds one directory per stream (HOUR), each with one directory per band holding that
band's manifest.jsonl and the score.txt that `score_ab.py BAND_DIR --center-hz 1500
--iq-flag=--negate-q > BAND_DIR/score.txt` wrote. Prints a Markdown table, one row per stream and
band: jt9 and our decoder per slot, our share of jt9's, t4/f2, the jt9 SNR median, and from the
manifest the LNA, clipped status words, the HL2's temperature and lost / late packets; then each
band's day, weighted by slots. build_four_band_run.py reads the same rows (hour_rows()).

The 2026-11-05 corpus README's Results table was printed with
  python3 tools/decode_bench/score_table.py ~/hl2_recordings/2026-11-05/four-band \\
      now 1500z 1600z 1700z 1800z 1900z 2000z 2100z 2200z 2300z
"""
import argparse
import datetime as dt
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
BAND_ORDER = ["10m", "12m", "15m", "17m", "20m", "30m", "40m", "60m", "80m", "160m"]
SCORE = re.compile(r"^\s*\S+\s+(\d+) slots\s+jt9\s+([\d.]+)/slot.*?ours\s+([\d.]+)/slot \(\s*([\d.]+)% of jt9\)"
                   r"\s+t4/f2\s+([\d.]+)/slot \(\s*([\d.]+)% of jt9\)\s+jt9 SNR median ([-+]?\d+) dB")


def read_score(path):
    """The one arm's summary line of a score_ab.py score.txt, or None if it has none."""
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = SCORE.match(line)
            if m:
                return {"slots": int(m.group(1)), "jt9": float(m.group(2)), "ours": float(m.group(3)),
                        "ours_pct": float(m.group(4)), "t4f2": float(m.group(5)), "t4f2_pct": float(m.group(6)),
                        "snr_median_db": int(m.group(7))}
    return None


def hour_rows(recording_dir, hours):
    """One dict per stream and band that has a score.txt, in stream then band order."""
    rows = []
    for hour in hours:
        hour_dir = os.path.join(recording_dir, hour)
        bands = sorted((b for b in os.listdir(hour_dir) if os.path.isfile(os.path.join(hour_dir, b, "score.txt"))),
                       key=lambda b: BAND_ORDER.index(b) if b in BAND_ORDER else 99)
        for band in bands:
            score = read_score(os.path.join(hour_dir, band, "score.txt"))
            if score is None:
                sys.exit(f"{hour_dir}/{band}/score.txt: no score_ab.py summary line")
            with open(os.path.join(hour_dir, band, "manifest.jsonl")) as f:
                man = [json.loads(line) for line in f if line.strip()]
            temps = [m["temp_c_min"] for m in man if m["temp_c_min"] is not None] + \
                    [m["temp_c_max"] for m in man if m["temp_c_max"] is not None]
            rows.append(dict(score, hour=hour, band=band, start_utc=man[0]["nominal_utc_start"],
                             lna_first_db=man[0]["lna_db_end"], lna_last_db=man[-1]["lna_db_end"],
                             clipped=sum(m["clipped_status_words"] for m in man),
                             status_words=sum(m["status_words"] for m in man),
                             temp_c=[min(temps), max(temps)] if temps else None,
                             lost=sum(m["lost_packets"] for m in man), late=sum(m["late_packets"] for m in man)))
    return rows


def day(rows, band):
    """A band's day from its hourly summaries: per slot weighted by slots, shares by jt9's decodes."""
    r = [x for x in rows if x["band"] == band]
    slots = sum(x["slots"] for x in r)
    jt9 = sum(x["jt9"] * x["slots"] for x in r)
    return {"band": band, "slots": slots, "jt9": jt9 / slots, "ours": sum(x["ours"] * x["slots"] for x in r) / slots,
            "ours_pct": sum(x["ours_pct"] * x["jt9"] * x["slots"] for x in r) / jt9,
            "t4f2_pct": sum(x["t4f2_pct"] * x["jt9"] * x["slots"] for x in r) / jt9,
            "lowest": min(r, key=lambda x: x["ours_pct"])}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording_dir")
    ap.add_argument("hours", nargs="*")
    ap.add_argument("--utc-offset-hours", type=float, default=-6)
    a = ap.parse_args()
    hours = a.hours or sorted(d for d in os.listdir(a.recording_dir) if os.path.isdir(os.path.join(a.recording_dir, d)))
    rows = hour_rows(a.recording_dir, hours)
    off = dt.timedelta(hours=a.utc_offset_hours)

    def local(s):
        t = dt.datetime.fromisoformat(s) + off
        return f"{t.hour % 12 or 12}:{t.minute:02d} {'AM' if t.hour < 12 else 'PM'}"

    def minus(n):
        return f"{n:+d}".replace("-", "−")

    print("| Stream (CST start) | Band | Slots | `jt9` per slot | Ours per slot (share of `jt9`) | t4/f2 | `jt9` SNR median "
          "| LNA | Clipped status words | HL2 °C | Lost / late packets |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in rows:
        lna = minus(r["lna_first_db"]) + (" → " + minus(r["lna_last_db"]) if r["lna_last_db"] != r["lna_first_db"] else "") + " dB"
        temp = f"{r['temp_c'][0]:.1f}–{r['temp_c'][1]:.1f}" if r["temp_c"] else "—"
        print(f"| `{r['hour']}` ({local(r['start_utc'])}) | {r['band'][:-1]} m | {r['slots']} | {r['jt9']:.1f} "
              f"| {r['ours']:.1f} ({r['ours_pct']:.1f}%) | {r['t4f2']:.1f} ({r['t4f2_pct']:.1f}%) | {minus(r['snr_median_db'])} dB "
              f"| {lna} | {r['clipped']:,} | {temp} | {r['lost']} / {r['late']} |")
    print()
    print("| Band | Slots | `jt9` per slot | Ours per slot | Ours, share of `jt9` | t4/f2, share | Lowest hour for ours |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for band in sorted({r["band"] for r in rows}, key=lambda b: BAND_ORDER.index(b) if b in BAND_ORDER else 99):
        d = day(rows, band)
        lo = d["lowest"]
        print(f"| {band[:-1]} m | {d['slots']:,} | {d['jt9']:.1f} | {d['ours']:.1f} | {d['ours_pct']:.1f}% | {d['t4f2_pct']:.1f}% "
              f"| `{lo['hour']}` ({local(lo['start_utc'])}): {lo['ours_pct']:.1f}% of {lo['jt9']:.1f} |")


if __name__ == "__main__":
    main()
