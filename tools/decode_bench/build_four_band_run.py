#!/usr/bin/env python3
"""Build one run of the site's four-band experiment from an HL2 recording and its sampled decodes.

  python3 tools/decode_bench/build_four_band_run.py RECORDING_DIR --decodes DECODES.jsonl \\
          --run-id ID --station-grid GRID [--station-label TEXT] [--hours HOUR ...] \\
          [--baseline CORPUS_DIR] [--scored-window BAND=DIR ...] \\
          [--lna-ab-score SCORE.txt --lna-ab-manifest MANIFEST.jsonl] [--corpus-url URL] [--note TEXT] \\
          [--utc-offset-hours -6 --tz-label CST] [--runs-dir webapp/experiments/four-band/runs]

RECORDING_DIR is a tools/hl2_record.py several-receiver recording: one directory per stream (HOUR),
each with one directory per band holding its manifest.jsonl. DECODES.jsonl is what sample_jt9.py
wrote for it. --hours keeps only those streams, for a run still recording: a stream not finished is
left out of both the radio's figures and the decodes.

--station-grid is the grid square the run listened from, four characters or six (a subsquare, a few
km across): distances are great-circle (haversine) from its centre to each heard square's centre,
in km. Heard squares are four characters, as FT8 messages carry them. A decode is placed by its
sender's grid square, learned from any message of that sender's in the run that carries one.

--baseline is the one-band survey corpus the run is compared with: its manifest-<band>.jsonl gives
each band's clock window, its README's Results table jt9 and our decoder per slot, the SNR median
and the LNA. --scored-window BAND=DIR gives this run's side for a band from score_ab.py's score.txt
in DIR (the band's slots on the survey's clock window, scored with jt9 and ours) and DIR's
manifest.jsonl; a band without one falls back to the sampled jt9 decodes in the window, if they
cover at least half of it. --lna-ab-score and --lna-ab-manifest are a `hl2_record.py --lna-ab`
run's score_ab.py --arm-field lna_arm_db output and its manifest.

Writes RUNS_DIR/ID/run.json, everything the page draws for the run, aggregated (not the raw rows),
and rewrites RUNS_DIR/index.json, the list of runs the page's picker and the experiments listing
read, from every RUNS_DIR/*/run.json. Commit both. Python 3, standard library.

Run 2026-11-05 was built, from the repository root, with
  python3 tools/decode_bench/build_four_band_run.py ~/hl2_recordings/2026-11-05/four-band \\
      --decodes ~/hl2_recordings/dashboard-2026-11-05/decodes.jsonl --run-id 2026-11-05 \\
      --station-grid EM10cg --station-label "Austin, Texas" \\
      --hours now 1500z 1600z 1700z 1800z 1900z 2000z 2100z 2200z 2300z \\
      --baseline tools/decode_bench/corpus/hl2-2026-11-04 \\
      --scored-window 10m=$HOME/hl2_recordings/2026-11-05/score-windows/10m \\
      --scored-window 15m=$HOME/hl2_recordings/2026-11-05/score-windows/15m \\
      --scored-window 20m=$HOME/hl2_recordings/2026-11-05/score-windows/20m \\
      --lna-ab-score ~/hl2_recordings/2026-11-05/lna-ab/score.txt \\
      --lna-ab-manifest ~/hl2_recordings/2026-11-05/lna-ab/40m/manifest.jsonl
"""
import argparse
import datetime as dt
import json
import math
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
DIST_BIN_KM = 500
EARTH_KM = 6371.0
FARTHEST = 5  # stations listed per band, each heard at least twice on it
SCORE_LINE = re.compile(r"^\s*(\S+)\s+(\d+) slots\s+jt9\s+([\d.]+)/slot.*?ours\s+([\d.]+)/slot \(\s*([\d.]+)% of jt9\)"
                        r".*?jt9 SNR median ([-+]?\d+) dB")
PAIRED_LINE = re.compile(r"paired (\S+) per turn \((\d+) pairs\): jt9 ([-+]?[\d.]+) ± ([\d.]+), ours ([-+]?[\d.]+) ± ([\d.]+)")


def band_of(dial_hz):
    """The amateur band a dial frequency is in, by its FT8 dial's nearest MHz."""
    mhz = {1.8: "160m", 3.5: "80m", 5.3: "60m", 7.0: "40m", 10.1: "30m", 14.0: "20m", 18.1: "17m", 21.0: "15m",
           24.9: "12m", 28.0: "10m", 50.3: "6m"}
    return mhz[min(mhz, key=lambda f: abs(f - dial_hz / 1e6))]


STATION_GRID = re.compile(r"^[A-Ra-r]{2}[0-9]{2}(?:[A-Xa-x]{2})?$")


def grid_centre(g):
    """(lat, lon) of a four- or six-character grid square's centre: a square is 1 x 2 degrees, a
    subsquare (the last two letters) 2.5 x 5 minutes, a few km."""
    g = g[:2].upper() + g[2:4] + g[4:].lower()
    lat = (ord(g[1]) - 65) * 10 - 90 + int(g[3])
    lon = (ord(g[0]) - 65) * 20 - 180 + int(g[2]) * 2
    if len(g) == 6:
        return (lat + (ord(g[5]) - 97 + 0.5) * 2.5 / 60, lon + (ord(g[4]) - 97 + 0.5) * 5 / 60)
    return (lat + 0.5, lon + 1)


def km_between(a, b):
    """Great-circle distance, haversine, km."""
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 2 * EARTH_KM * math.asin(math.sqrt(h))


def read_score(path):
    """score_ab.py's summary: ({label: {slots, jt9, ours, ours_pct, snr_median_db}}, paired or None)."""
    arms, paired = {}, None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = SCORE_LINE.match(line)
            if m:
                arms[m.group(1)] = {"slots": int(m.group(2)), "jt9": float(m.group(3)), "ours": float(m.group(4)),
                                    "ours_pct": float(m.group(5)), "snr_median_db": int(m.group(6))}
            m = PAIRED_LINE.search(line)
            if m:
                paired = {"pairs": int(m.group(2)), "jt9": [float(m.group(3)), float(m.group(4))],
                          "ours": [float(m.group(5)), float(m.group(6))]}
    if not arms:
        sys.exit(f"{path}: no score_ab.py summary lines")
    return arms, paired


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


def survey_results(readme):
    """{band: {jt9, ours_pct, snr_median_db, lna}} from the corpus README's Results table."""
    out, cols = {}, None
    with open(readme, encoding="utf-8") as f:
        for line in f:
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if cols is None:
                if "`jt9` per slot" in cells:
                    cols = cells
                continue
            if not line.startswith("|"):
                break
            m = re.match(r"^(\d+) m$", cells[0])
            if not m:
                continue
            row = dict(zip(cols, cells))
            ours = re.search(r"\(([\d.]+)%\)", row.get("Ours per slot (share of `jt9`)", ""))
            snr = re.match(r"^([-+−]?\d+) dB$", row.get("`jt9` SNR median", ""))
            out[m.group(1) + "m"] = {"jt9": float(row["`jt9` per slot"]), "ours_pct": float(ours.group(1)) if ours else None,
                                     "snr_median_db": int(snr.group(1).replace("−", "-")) if snr else None,
                                     "lna": row.get("LNA", "").replace(" dB", "").replace(" → ", "→") or None}
    if not out:
        sys.exit(f"{readme}: no Results table with a `jt9` per slot column")
    return out


def lna_span(recs):
    """'+8' or '+17→+14': the LNA at the first slot's end and the last's."""
    a, b = recs[0]["lna_db_end"], recs[-1]["lna_db_end"]
    return f"{a:+d}" if a == b else f"{a:+d}→{b:+d}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording_dir")
    ap.add_argument("--decodes", required=True, help="sample_jt9.py's output for this recording")
    ap.add_argument("--run-id", required=True, help="the run's folder name, its date: YYYY-MM-DD")
    ap.add_argument("--hours", nargs="+", help="stream directories to include (default: all)")
    ap.add_argument("--station-grid", required=True, help="the four- or six-character grid square the run listened from")
    ap.add_argument("--station-label", help="where that is, in words")
    ap.add_argument("--baseline", help="the one-band survey corpus directory to compare with")
    ap.add_argument("--scored-window", action="append", default=[], metavar="BAND=DIR",
                    help="score_ab.py's score.txt and the manifest for BAND's slots on the survey's clock window")
    ap.add_argument("--lna-ab-score", help="score_ab.py --arm-field lna_arm_db output for an --lna-ab run")
    ap.add_argument("--lna-ab-manifest", help="that run's manifest.jsonl")
    ap.add_argument("--corpus-url", help="this run's own corpus, once it is released")
    ap.add_argument("--note", help="one line the page shows with the run, such as what a snapshot leaves out")
    ap.add_argument("--utc-offset-hours", type=float, default=-6, help="the page's local time (CST: -6)")
    ap.add_argument("--tz-label", default="CST")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--runs-dir", default=os.path.normpath(os.path.join(here, "..", "..", "webapp", "experiments", "four-band", "runs")))
    a = ap.parse_args()

    if not re.match(r"^\d{4}-\d{2}-\d{2}$", a.run_id):
        sys.exit("--run-id is a date, YYYY-MM-DD")
    if not STATION_GRID.match(a.station_grid):
        sys.exit("--station-grid is a four- or six-character grid square, such as EM10 or EM10cg")
    a.station_grid = a.station_grid[:2].upper() + a.station_grid[2:4] + a.station_grid[4:].lower()
    home = grid_centre(a.station_grid)
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
    heard = []  # (band, sender or None, snr) per decode, placed once every sender's grid is known
    call_grids = {}  # sender -> {grid: messages carrying it}
    for d in dec:
        per_slot.append((d["band"], minute(utc(d["utc"])), len(d["decodes"])))
        for s, _hz, msg in d["decodes"]:
            words = msg.split()
            if not words:
                continue
            snr[d["band"]][s] = snr[d["band"]].get(s, 0) + 1
            call = sender(words)
            heard.append((d["band"], call, s))
            if call:
                calls[d["band"]].add(call)
            else:
                unknown[d["band"]] += 1
            g = words[-1]
            if GRID.match(g) and g != "RR73":
                if call:
                    cg = call_grids.setdefault(call, {})
                    cg[g] = cg.get(g, 0) + 1
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

    # ---- How far: each decode placed at its sender's grid square (the one it sent most often).
    grid_of = {c: max(gs.items(), key=lambda kv: (kv[1], kv[0]))[0] for c, gs in call_grids.items()}
    km_of_grid = {}

    def km(g):
        if g not in km_of_grid:
            km_of_grid[g] = round(km_between(home, grid_centre(g)))
        return km_of_grid[g]

    dist = {"bin_km": DIST_BIN_KM, "bands": {}}
    for b in bands:
        rows = [(km(grid_of[c]), s, c) for bb, c, s in heard if bb == b and c in grid_of]
        hist, cells, by_bin, far = {}, {}, {}, {}
        for k, s, c in rows:
            i = k // DIST_BIN_KM * DIST_BIN_KM
            hist[i] = hist.get(i, 0) + 1
            cells[(i, s)] = cells.get((i, s), 0) + 1
            by_bin.setdefault(i, []).append(s)
            f = far.setdefault(c, [c, grid_of[c], k, s, 0])
            f[3] = max(f[3], s)
            f[4] += 1
        dist["bands"][b] = {
            "decodes": sum(1 for x in heard if x[0] == b), "placed": len(rows),
            "median_km": round(st.median(r[0] for r in rows)) if rows else None,
            "hist": sorted([k, n] for k, n in hist.items()),
            "snr_cells": sorted([k, s, n] for (k, s), n in cells.items()),
            "snr_median": [[k, st.median(v), len(v)] for k, v in sorted(by_bin.items())],
            # A station heard once may be a false decode (jt9 -d 3 makes a few): list those heard twice or more.
            "farthest": [f[:4] for f in sorted((f for f in far.values() if f[4] >= 2), key=lambda f: (-f[2], f[0]))[:FARTHEST]],
        }

    heat = {}
    for b, m, n in per_slot:
        heat.setdefault((b, int(m // 60)), []).append(n)
    trend = {}
    for b, m, n in per_slot:
        trend.setdefault(b, {}).setdefault(int(m // 10) * 10, []).append(n)

    # ---- The baseline: the one-band survey against this run over the same clock window, from a
    # scored window where there is one (jt9 and ours), else from the sampled jt9 decodes.
    windows = {}
    for spec in a.scored_window:
        band, _, d = spec.partition("=")
        arms, _ = read_score(os.path.join(d, "score.txt"))
        arm = next(iter(arms.values()))
        recs = read_jsonl(os.path.join(d, "manifest.jsonl"))
        windows[band] = dict(arm, lna=lna_span(recs), source="scored")
    baseline = None
    if a.baseline:
        per = survey_results(os.path.join(a.baseline, "README.md"))
        every = round(len(slots) * len(bands) / len(dec))
        rows, first = [], None
        for b in bands:
            man = os.path.join(a.baseline, f"manifest-{b}.jsonl")
            if b not in per or not os.path.isfile(man):
                continue
            starts = sorted(utc(r["nominal_utc_start"]) for r in read_jsonl(man))
            first = min(first or starts[0], starts[0])
            lo, hi = [((t + off) - (t + off).replace(hour=0, minute=0, second=0, microsecond=0)).total_seconds() / 60
                      for t in (starts[0], starts[-1])]
            four = windows.get(b)
            if four is None:
                v = [n for bb, m, n in per_slot if bb == b and lo <= m % 1440 <= hi]
                if len(v) * every * 2 >= len(starts):  # the sampled slots cover at least half the window
                    four = {"slots": len(v), "jt9": mean(v), "ours": None, "ours_pct": None, "snr_median_db": None,
                            "lna": None, "source": "sampled"}
            rows.append({"band": b, "window": [hhmm(starts[0]), hhmm(starts[-1] + dt.timedelta(seconds=SLOT_S))],
                         "one_band": dict(per[b], slots=len(starts)), "four_band": four})
        name = os.path.basename(os.path.normpath(a.baseline))
        rel = os.path.relpath(os.path.abspath(a.baseline), os.path.join(here, "..", ".."))
        baseline = {"corpus": name, "url": REPO_URL + rel.replace(os.sep, "/") + "/README.md",
                    "date": (first + off).strftime("%Y-%m-%d") if first else None, "bands": rows}

    # ---- The LNA A/B: score_ab.py's arms and pairs, and each arm's clipping from its manifest.
    lna_ab = None
    if a.lna_ab_score and a.lna_ab_manifest:
        arms, paired = read_score(a.lna_ab_score)
        recs = read_jsonl(a.lna_ab_manifest)
        ab_starts = sorted(utc(r["nominal_utc_start"]) for r in recs)
        out_arms = []
        for label, arm in arms.items():
            db = int(label.split("=")[1])
            v = [r for r in recs if r.get("lna_arm_db") == db]
            out_arms.append(dict(arm, lna_db=db, clipped_median=st.median(r["clipped_status_words"] for r in v),
                                 clipped_share_median=round(st.median(r["clipped_status_words"] / r["status_words"] for r in v), 3),
                                 status_words_median=st.median(r["status_words"] for r in v)))
        lna_ab = {"band": band_of(recs[0]["dial_hz"]), "dial_hz": recs[0]["dial_hz"], "receivers": recs[0]["receivers"],
                  "window": [hhmm(ab_starts[0]), hhmm(ab_starts[-1] + dt.timedelta(seconds=SLOT_S))],
                  "slots": len(recs), "turn_slots": next((i for i, r in enumerate(recs) if r.get("lna_arm_db") != recs[0].get("lna_arm_db")), None),
                  "arms": out_arms, "paired": paired}

    run = {
        "id": a.run_id,
        "date": a.run_id,
        "tz": {"label": a.tz_label, "utc_offset_min": round(a.utc_offset_hours * 60)},
        "covers": {"streams": hours, "first_utc": t0.strftime("%Y-%m-%dT%H:%M:%SZ"),
                   "end_utc": t_end.strftime("%Y-%m-%dT%H:%M:%SZ"), "first_local": hhmm(t0), "end_local": hhmm(t_end)},
        "note": a.note,
        "station": {"grid": a.station_grid, "label": a.station_label, "lat": round(home[0], 4), "lon": round(home[1], 4)},
        "corpus_url": a.corpus_url,
        "ours_scored": False,  # the day's sampled decodes are jt9's; ours scores only the windows and the A/B
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
        "grids": sorted((list(e) + [km(e[0])] for e in grids.values()), key=lambda e: (-e[2], e[0], e[1])),
        "distance": dist,
        "lna_ab": lna_ab,
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
