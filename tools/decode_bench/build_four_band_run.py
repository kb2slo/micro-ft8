#!/usr/bin/env python3
"""Build one run of the site's four-band experiment from an HL2 recording and its sampled decodes.

  python3 tools/decode_bench/build_four_band_run.py RECORDING_DIR --decodes DECODES.jsonl \\
          --run-id ID [--hours HOUR ...] [--baseline CORPUS_DIR] [--corpus-url URL] [--note TEXT] \\
          [--utc-offset-hours -6 --tz-label CST] [--runs-dir webapp/experiments/four-band/runs]

RECORDING_DIR is a tools/hl2_record.py several-receiver recording: one directory per stream (HOUR),
each with one directory per band holding its manifest.jsonl. DECODES.jsonl is what sample_jt9.py
wrote for it. --hours keeps only those streams, for a run still recording: a stream not finished is
left out of both the radio's figures and the decodes. --baseline is the one-band survey corpus the
run is compared with (its manifest-<band>.jsonl for each band's clock window, its README's Results
table for jt9 decodes per slot).

Writes RUNS_DIR/ID/run.json, everything the page draws for the run, aggregated (a few tens of KB,
not the raw rows), and rewrites RUNS_DIR/index.json, the list of runs the page's picker and the
experiments listing read, from every RUNS_DIR/*/run.json. Commit both. Python 3, standard library.

Run 2026-11-05 was built, from the repository root, with
  python3 tools/decode_bench/build_four_band_run.py ~/hl2_recordings/2026-11-05/four-band \\
      --decodes ~/hl2_recordings/dashboard-2026-11-05/decodes.jsonl --run-id 2026-11-05 \\
      --hours now 1500z 1600z 1700z 1800z 1900z 2000z 2100z \\
      --baseline tools/decode_bench/corpus/hl2-2026-11-04 \\
      --note "A snapshot of the day so far: the recording ran on until 7:30 PM CST, and the hours after 4:01 PM CST are not in it yet."
"""
import argparse
import datetime as dt
import json
import os
import re
import statistics as st
import sys

SLOT_S = 15
GRID = re.compile(r"^[A-R]{2}[0-9]{2}$")
# A well-formed callsign: an ITU prefix (one or two letters, a letter and a digit, or a digit and a
# letter), a digit, a suffix of one to seven characters ending in a letter (four for most calls,
# up to seven for a special-event station such as PF40EUDXF); optionally a portable
# prefix or suffix (VE3/W1ABC, W1ABC/P). jt9 brackets a call sent as a hash: <W1ABC> is one it
# resolved, <...> one it could not.
CALL = re.compile(r"^(?:[A-Z0-9]{1,4}/)?(?:[A-Z]{1,2}|[A-Z][0-9]|[0-9][A-Z])[0-9][A-Z0-9]{0,6}[A-Z](?:/[A-Z0-9]{1,4})?$")
CQ_MODIFIER = re.compile(r"^(?:[A-Z]{1,4}|[0-9]{3})$")  # CQ DX, CQ POTA, CQ 290
BAND_ORDER = ["160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"]
REPO_URL = "https://github.com/kb2slo/micro-ft8/blob/main/"


def utc(s):
    return dt.datetime.fromisoformat(s.replace("Z", "+00:00"))


def band_key(b):
    return -BAND_ORDER.index(b) if b in BAND_ORDER else 0  # 10 m first, as the receivers are numbered


def read_jsonl(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def callsign(word):
    """The word as a callsign if it is a well-formed one (brackets of a resolved hash removed), else None."""
    w = word[1:-1] if word.startswith("<") and word.endswith(">") else word
    return w if CALL.match(w) else None


def sender(words):
    """The sending call, or None when it is not a well-formed callsign: the word after CQ (or after
    CQ and one modifier), or the second word of 'TO FROM ...'. None covers an unresolved hash
    (<...>), free text, and any message whose sender cannot be told."""
    if words[0] == "CQ":
        if len(words) >= 2 and callsign(words[1]):
            return callsign(words[1])
        if len(words) >= 3 and CQ_MODIFIER.match(words[1]):
            return callsign(words[2])
        return None
    return callsign(words[1]) if len(words) >= 2 else None


def survey_jt9_per_slot(readme):
    """{band: jt9 decodes per slot} from the corpus README's Results table."""
    out, col = {}, None
    with open(readme, encoding="utf-8") as f:
        for line in f:
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if col is None:
                if any(c == "`jt9` per slot" for c in cells):
                    col = cells.index("`jt9` per slot")
                continue
            if not line.startswith("|"):
                break
            m = re.match(r"^(\d+) m$", cells[0])
            if m:
                out[m.group(1) + "m"] = float(cells[col])
    if not out:
        sys.exit(f"{readme}: no Results table with a `jt9` per slot column")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording_dir")
    ap.add_argument("--decodes", required=True, help="sample_jt9.py's output for this recording")
    ap.add_argument("--run-id", required=True, help="the run's folder name, its date: YYYY-MM-DD")
    ap.add_argument("--hours", nargs="+", help="stream directories to include (default: all)")
    ap.add_argument("--baseline", help="the one-band survey corpus directory to compare with")
    ap.add_argument("--corpus-url", help="this run's own corpus, once it is released")
    ap.add_argument("--note", help="one line the page shows with the run, such as what a snapshot leaves out")
    ap.add_argument("--utc-offset-hours", type=float, default=-6, help="the page's local time (CST: -6)")
    ap.add_argument("--tz-label", default="CST")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--runs-dir", default=os.path.normpath(os.path.join(here, "..", "..", "webapp", "experiments", "four-band", "runs")))
    a = ap.parse_args()

    if not re.match(r"^\d{4}-\d{2}-\d{2}$", a.run_id):
        sys.exit("--run-id is a date, YYYY-MM-DD")
    off = dt.timedelta(hours=a.utc_offset_hours)
    hours = a.hours or sorted(d for d in os.listdir(a.recording_dir) if os.path.isdir(os.path.join(a.recording_dir, d)))

    # ---- The radio's day: every slot, radio-wide. The receivers share one LNA, one ADC and one
    # stream, so every band's manifest carries the same figures; the first band's stands for all.
    bands, radio = set(), {}
    for hour in hours:
        hour_dir = os.path.join(a.recording_dir, hour)
        hour_bands = sorted((b for b in os.listdir(hour_dir) if os.path.isfile(os.path.join(hour_dir, b, "manifest.jsonl"))), key=band_key)
        if not hour_bands:
            sys.exit(f"{hour_dir}: no band manifests")
        bands.update(hour_bands)
        for rec in read_jsonl(os.path.join(hour_dir, hour_bands[0], "manifest.jsonl")):
            radio[rec["nominal_utc_start"]] = rec
    bands = sorted(bands, key=band_key)
    slots = sorted(radio.values(), key=lambda r: r["nominal_utc_start"])
    t0 = utc(slots[0]["nominal_utc_start"])
    t_end = utc(slots[-1]["nominal_utc_start"]) + dt.timedelta(seconds=SLOT_S)
    local_midnight = (t0 + off).replace(hour=0, minute=0, second=0, microsecond=0) - off

    def minute(t):  # local minutes since the run's first local midnight
        return (t - local_midnight).total_seconds() / 60

    def hhmm(t):
        return (t + off).strftime("%H:%M")

    radio_out = {"t0_utc": t0.strftime("%Y-%m-%dT%H:%M:%SZ"), "slot_s": SLOT_S, "start_min": round(minute(t0), 2),
                 "i": [], "lna_db": [], "clipped": [], "temp_c": []}
    for r in slots:
        radio_out["i"].append(round((utc(r["nominal_utc_start"]) - t0).total_seconds() / SLOT_S))
        radio_out["lna_db"].append(r["lna_db_end"])
        radio_out["clipped"].append(r["clipped_status_words"])
        radio_out["temp_c"].append(r["temp_c_max"])
    temps = [r["temp_c_max"] for r in slots if r["temp_c_max"] is not None]

    # ---- The sampled jt9 decodes, in the hours kept.
    keep = set(hours)
    dec = sorted((d for d in read_jsonl(a.decodes) if d["hour"] in keep and d["band"] in bands), key=lambda d: d["utc"])
    if not dec:
        sys.exit(f"{a.decodes}: no decodes in hours {' '.join(hours)}")
    per_slot = []  # (band, local minute, decodes)
    grids, calls = {}, {b: set() for b in bands}
    unknown = {b: 0 for b in bands}  # messages whose sender is not a well-formed callsign
    snr = {b: {} for b in bands}
    for d in dec:
        per_slot.append((d["band"], minute(utc(d["utc"])), len(d["decodes"])))
        for s, _hz, msg in d["decodes"]:
            words = msg.split()
            if not words:
                continue
            snr[d["band"]][s] = snr[d["band"]].get(s, 0) + 1
            call = sender(words)
            if call:
                calls[d["band"]].add(call)
            else:
                unknown[d["band"]] += 1
            g = words[-1]
            if GRID.match(g) and g != "RR73":
                e = grids.setdefault((g, d["band"]), [g, d["band"], 0, -99])
                e[2] += 1
                e[3] = max(e[3], s)

    def mean(v):
        return round(st.mean(v), 2) if v else None

    per_band = []
    for b in bands:
        n = [x[2] for x in per_slot if x[0] == b]
        s = [k for k, c in snr[b].items() for _ in range(c)]
        per_band.append({"band": b, "slots_sampled": len(n), "decodes": sum(n), "decodes_per_slot": mean(n),
                         "stations": len(calls[b]), "unknown_sender_messages": unknown[b], "grid_squares": len({g for g, gb in grids if gb == b}),
                         "snr_median_db": st.median(s) if s else None})
    all_snr = [k for b in bands for k, c in snr[b].items() for _ in range(c)]

    heat = {}
    for b, m, n in per_slot:
        heat.setdefault((b, int(m // 60)), []).append(n)
    trend = {}
    for b, m, n in per_slot:
        trend.setdefault(b, {}).setdefault(int(m // 10) * 10, []).append(n)

    # ---- The baseline: the one-band survey's jt9 per slot against this run's over the same clock window.
    baseline = None
    if a.baseline:
        per = survey_jt9_per_slot(os.path.join(a.baseline, "README.md"))
        rows, first = [], None
        for b in bands:
            man = os.path.join(a.baseline, f"manifest-{b}.jsonl")
            if b not in per or not os.path.isfile(man):
                continue
            starts = sorted(utc(r["nominal_utc_start"]) for r in read_jsonl(man))
            first = min(first or starts[0], starts[0])
            lo, hi = [((t + off) - (t + off).replace(hour=0, minute=0, second=0, microsecond=0)).total_seconds() / 60
                      for t in (starts[0], starts[-1])]
            v = [n for bb, m, n in per_slot if bb == b and lo <= m % 1440 <= hi]
            rows.append({"band": b, "window": [hhmm(starts[0]), hhmm(starts[-1] + dt.timedelta(seconds=SLOT_S))],
                         "one_band": per[b], "one_band_slots": len(starts), "four_band": mean(v), "four_band_slots": len(v)})
        name = os.path.basename(os.path.normpath(a.baseline))
        rel = os.path.relpath(os.path.abspath(a.baseline), os.path.join(here, "..", ".."))
        baseline = {"corpus": name, "url": REPO_URL + rel.replace(os.sep, "/") + "/README.md",
                    "date": (first + off).strftime("%Y-%m-%d") if first else None, "bands": rows}

    run = {
        "id": a.run_id,
        "date": a.run_id,
        "tz": {"label": a.tz_label, "utc_offset_min": round(a.utc_offset_hours * 60)},
        "covers": {"streams": hours, "first_utc": t0.strftime("%Y-%m-%dT%H:%M:%SZ"),
                   "end_utc": t_end.strftime("%Y-%m-%dT%H:%M:%SZ"), "first_local": hhmm(t0), "end_local": hhmm(t_end)},
        "note": a.note,
        "corpus_url": a.corpus_url,
        "ours_scored": False,  # this tool takes jt9's decodes only; our decoder's score is a corpus's
        "sampling": {"every": round(len(slots) * len(bands) / len(dec)), "decoder": "jt9 -8 -d 3"},
        "bands": bands,
        "totals": {"slots_recorded": len(slots), "slots_sampled": len(dec), "decodes": sum(x[2] for x in per_slot),
                   "stations": len(set().union(*calls.values())), "unknown_sender_messages": sum(unknown.values()), "grid_squares": len({g for g, _ in grids}),
                   "snr_median_db": st.median(all_snr) if all_snr else None,
                   "lost_packets": sum(r["lost_packets"] for r in slots), "late_packets": sum(r["late_packets"] for r in slots),
                   "temp_c": [min(temps), max(temps)] if temps else None},
        "per_band": per_band,
        "heat": [{"band": b, "hour": h, "mean": mean(v), "slots": len(v)} for (b, h), v in sorted(heat.items(), key=lambda kv: (band_key(kv[0][0]), kv[0][1]))],
        "trend": {"bin_min": 10, "bands": {b: [[m, mean(v)] for m, v in sorted(trend.get(b, {}).items())] for b in bands}},
        "snr": {b: sorted([k, c] for k, c in snr[b].items()) for b in bands},
        "grids": sorted((list(e) for e in grids.values()), key=lambda e: (-e[2], e[0], e[1])),
        "radio": radio_out,
        "baseline": baseline,
    }

    out_dir = os.path.join(a.runs_dir, a.run_id)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "run.json")
    with open(path, "w") as f:
        json.dump(run, f, separators=(",", ":"))
        f.write("\n")
    print(f"{path}: {os.path.getsize(path):,} bytes; {len(slots)} slots, {len(dec)} sampled, "
          f"{run['covers']['first_local']}-{run['covers']['end_local']} {a.tz_label}")

    runs = []
    for rid in sorted(os.listdir(a.runs_dir), reverse=True):
        p = os.path.join(a.runs_dir, rid, "run.json")
        if os.path.isfile(p):
            with open(p) as f:
                r = json.load(f)
            runs.append({"id": r["id"], "date": r["date"], "covers": r["covers"]["first_local"] + "–" + r["covers"]["end_local"] + " " + r["tz"]["label"]})
    with open(os.path.join(a.runs_dir, "index.json"), "w") as f:
        json.dump({"runs": runs}, f, indent=1)
        f.write("\n")
    print(f"{os.path.join(a.runs_dir, 'index.json')}: {len(runs)} runs")


if __name__ == "__main__":
    main()
