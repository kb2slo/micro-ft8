#!/usr/bin/env python3
"""FT4 in a QMX I/Q recording: jt9 -5 on each 7.5 s period, decodes binned by audio offset.

The 2026-11-02 QMX corpus (tools/decode_bench/corpus/qmx-2026-11-02) was recorded with the dial on
FT8's, so on 20 m FT4 (14.080 MHz) sits 19.5 kHz above the LO, near the top of the radio's 48 kHz
window. This decodes it there, to see whether it decodes at all and whether decodes thin out toward
the window's edge (row an8x, the receive dial; docs/roadmap/several-channels.md, Open).

  python3 tools/decode_bench/score_ft4_edge.py BAND_DIR [--center-hz 19500] [--ours [--costs]] [decoder flags...]

Each I/Q slot in BAND_DIR's manifest.jsonl goes through tools/iq_convert at --center-hz, 12 kHz out,
then is cut into its two 7.5 s FT4 periods for `jt9 -5 -d 3`. With --ours, the same periods also go to
tools/decode_recall/decode_recall --mode ft4 (the firmware's decode, row an8x) and its messages are
scored against jt9's: found, and extras (not all false: jt9 misses some). Each period at the firmware's
start and budget for it beside FT8 (decode_limits.h): the first at DECODE_TRIGGER_FT4 and DECODE_BUDGET_FT4,
the second, which must be done before FT8 starts, at DECODE_TRIGGER_FT4_BESIDE_FT8 and
DECODE_BUDGET_FT4_BESIDE_FT8; decoder flags that follow replace both.
--costs also times our decoder on each slot as FT8 (its channel at 13,500 Hz) and on its FT4 periods,
no budget, one file at a time, and prints what an FT4 search and an FT4 LDPC iteration cost against
FT8's on this machine: the ratio DECODE_COST_SEARCH_FT4 is scaled from. Needs `jt9` on PATH and tools/iq_convert built (and
tools/decode_recall with --ours). Work files go in a temporary directory and are removed.
"""
import argparse
import json
import sys
import os
import re
import statistics as st
import subprocess
import tempfile
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONV = os.path.join(ROOT, "tools", "iq_convert", "iq_convert")
DECODER = os.path.join(ROOT, "tools", "decode_recall", "decode_recall")
OURS = re.compile(r"^000000\s+(-?\d+)\s+\S+\s+(\d+)\s+~\s+(.*?)\s*$")
sys.path.insert(0, os.path.join(ROOT, "tools", "decode_recall"))
from report import normalise  # noqa: E402  the recall harness's: hashed calls and spacing
LINE = re.compile(r"^\s*\d{4,6}\s+([-+]?\d+)\s+([-+]?\d+\.\d+)\s+(\d+)\s+\+\s+(.*?)\s*$")
PERIOD_BYTES = 90000 * 2  # 7.5 s of 16-bit mono at 12 kHz


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("band_dir")
    ap.add_argument("--center-hz", type=float, default=19500, help="the FT4 channel above the LO")
    ap.add_argument("--ours", action="store_true", help="also score the firmware's FT4 decode against jt9")
    ap.add_argument("--costs", action="store_true", help="with --ours, time FT4 against FT8 on this machine")
    a, decoder_flags = ap.parse_known_args()
    recs = [json.loads(l) for l in open(os.path.join(a.band_dir, "manifest.jsonl"))]
    iq = [r for r in recs if r["mode_tag"] == "iq"]
    rows = []
    truth = {}   # period wav -> jt9's normalised messages
    with tempfile.TemporaryDirectory() as tmp:
        for i, r in enumerate(iq):
            wav12 = os.path.join(tmp, "conv.wav")
            subprocess.run([CONV, "--center-hz", str(a.center_hz), "--output-rate", "12000", "--normalize-db", "-20",
                            "-o", wav12, os.path.join(a.band_dir, r["file"])], capture_output=True, check=True)
            if a.costs:
                subprocess.run([CONV, "--center-hz", "13500", "--output-rate", "12000", "--normalize-db", "-20",
                                "-o", os.path.join(tmp, f"ft8_s{i:03d}.wav"), os.path.join(a.band_dir, r["file"])],
                               capture_output=True, check=True)
            w = wave.open(wav12)
            data = w.readframes(w.getnframes())
            w.close()
            for k in (0, 1):
                name = f"s{i:03d}p{k}.wav"
                o = wave.open(os.path.join(tmp, name), "wb")
                o.setnchannels(1)
                o.setsampwidth(2)
                o.setframerate(12000)
                o.writeframes(data[k * PERIOD_BYTES:(k + 1) * PERIOD_BYTES])
                o.close()
                out = subprocess.run(["jt9", "-5", "-d", "3", name], cwd=tmp, capture_output=True, text=True).stdout
                truth[name] = set()
                for line in out.splitlines():
                    m = LINE.match(line)
                    if m:
                        rows.append((r["file"], k, int(m.group(1)), float(m.group(2)), int(m.group(3))))
                        truth[name].add(normalise(m.group(4)))
        if a.ours:
            score_ours(tmp, truth, decoder_flags)
            if a.costs:
                score_costs(tmp, sorted(truth))
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


def limit(name):
    header = open(os.path.join(ROOT, "components", "decode_pipeline", "include", "decode_limits.h")).read()
    return int(re.search(rf"#define {name} (\d+)", header).group(1))


def decode(paths, flags):
    """Per file: our normalised messages, and the END line's fields."""
    out = subprocess.run([DECODER, "--mode", "ft4"] + flags + paths, capture_output=True, text=True, check=True).stdout
    ours, ends, current = {}, {}, None
    for line in out.splitlines():
        if line.startswith("FILE "):
            current = os.path.basename(line[5:])
            ours[current] = set()
        elif line.startswith("END "):
            ends[current] = {k: int(v) for k, v in (kv.split("=") for kv in line[4:].split())}
        else:
            m = OURS.match(line)
            if m and current:
                ours[current].add(normalise(m.group(3)))
    return ours, ends


def score_ours(tmp, truth, flags):
    # Each half where the firmware decodes it beside FT8 (decode_budget.h): the first at FT4's start and budget,
    # the second earlier, with what is left before FT8 starts.
    halves = (("p0", "DECODE_BUDGET_FT4", "DECODE_TRIGGER_FT4"),
              ("p1", "DECODE_BUDGET_FT4_BESIDE_FT8", "DECODE_TRIGGER_FT4_BESIDE_FT8"))
    for half, budget_name, trigger_name in halves:
        names = [n for n in sorted(truth) if n.endswith(half + ".wav")]
        use = flags or ["--budget-units", str(limit(budget_name)), "--trigger-blocks", str(limit(trigger_name))]
        ours, ends = decode([os.path.join(tmp, n) for n in names], use)
        found = sum(len(truth[n] & ours.get(n, set())) for n in names)
        total = sum(len(truth[n]) for n in names)
        extras = sum(len(ours.get(n, set()) - truth[n]) for n in names)
        units = [e["units"] for e in ends.values()]
        label = "first" if half == "p0" else "second"
        print(f"ours, {label} periods ({' '.join(use)}): {found}/{total} of jt9's ({100 * found / max(total, 1):.1f}%), "
              f"{extras} extras; units median {st.median(units)}, max {max(units)}, cut {sum(e['cut'] for e in ends.values())}")


def fit(xs, ys):
    mx, my = st.mean(xs), st.mean(ys)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return my - slope * mx, slope


def score_costs(tmp, ft4_names):
    """Each file decoded alone, so the timings do not share the machine."""
    def run(mode, names):
        rows = []
        for n in names:
            out = subprocess.run([DECODER, "--mode", mode, "--budget-units", "0", os.path.join(tmp, n)],
                                 capture_output=True, text=True, check=True).stdout
            end = [l for l in out.splitlines() if l.startswith("END ")][0]
            rows.append({k: int(v) for k, v in (kv.split("=") for kv in end[4:].split())})
        return rows
    ft8 = run("ft8", sorted(n for n in os.listdir(tmp) if n.startswith("ft8_")))
    ft4 = run("ft4", ft4_names)
    costs = {}
    for mode, rows in (("FT8", ft8), ("FT4", ft4)):
        search = st.median(r["find_us"] / r["searches"] for r in rows)
        _, iteration = fit([r["iters"] for r in rows], [r["decode_us"] for r in rows])
        costs[mode] = (search, iteration)
        print(f"{mode} on this machine: a search {search:.0f} us, an LDPC iteration {iteration * 1000:.0f} ns ({len(rows)} files)")
    s8, i8 = costs["FT8"]
    s4, i4 = costs["FT4"]
    print(f"FT4 against FT8: a search {s4 / s8:.2f}, an iteration {i4 / i8:.2f}; "
          f"DECODE_COST_SEARCH {limit('DECODE_COST_SEARCH')} scaled is {limit('DECODE_COST_SEARCH') * (s4 / s8) / (i4 / i8):.0f}")


if __name__ == "__main__":
    main()
