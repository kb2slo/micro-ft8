#!/usr/bin/env python3
"""Where a mode's decode should start, at the budget two workers fit in what is left
(docs/architecture/decode.md, Decoding later in the slot).

A later start sees more of each signal's tail but leaves less time before the deadline, so each start is
scored at the units the P4's two workers fit in what remains: decode_bench's fit, 14 ms + 0.2123 ms a unit
(fit_units.py, 2026-11-07), with the 54 ms margin that puts FT8's 79 blocks at DECODE_BUDGET_UNITS. FT8 is
scored on both corpora against their jt9 truth, and on --capture dirs of 6 kHz slots by message count. FT4
is scored on --ft4 dirs of 7.5 s periods at 12 kHz against `jt9 -5` truth, cached beside them; its
deadline is its own period's end, so this is the first period, which decodes alone.

    python3 tools/decode_recall/sweep_start.py [--capture DIR ...] [--ft4 DIR]
"""

import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402

FIXED_MS = 14.0
MS_PER_UNIT = 0.2123
MARGIN_MS = 54.0
MODES = {  # slot or period ms, block ms, the firmware's trigger in blocks, starts to try
    "ft8": (15000, 160, 79, [77, 79, 81, 83, 85, 87]),
    "ft4": (7500, 48, 105, [100, 105, 110, 115, 120, 125, 130]),
}
JT9_LINE = re.compile(r"^\s*\d{4,6}\s+[-+]?\d+\s+[-+]?\d+\.\d+\s+\d+\s+\+\s+(.*?)\s*$")


def budget(mode, blocks):
    slot_ms, block_ms, _, _ = MODES[mode]
    left = slot_ms - blocks * block_ms - MARGIN_MS - FIXED_MS
    return max(0, int(left / MS_PER_UNIT))


def decode(files, mode, blocks):
    args = [str(report.DECODER), "--mode", mode, "--trigger-blocks", str(blocks),
            "--budget-units", str(budget(mode, blocks))]
    out = subprocess.run(args + [str(f) for f in files], capture_output=True, text=True, check=True).stdout
    found, current = {}, None
    for line in out.splitlines():
        if line.startswith("FILE "):
            current = line[5:]
            found[current] = set()
        elif line.startswith("000000") and current:
            found[current].add(report.normalise(line.split("~", 1)[1]))
    return found


def ft4_truth(directory):
    cache = Path(directory) / "jt9_truth.json"
    if cache.is_file():
        return json.loads(cache.read_text())
    truth = {}
    for wav in sorted(Path(directory).glob("*.wav")):
        out = subprocess.run(["jt9", "-5", "-d", "3", wav.name], cwd=directory, capture_output=True, text=True).stdout
        truth[str(wav)] = [report.normalise(m.group(1)) for m in map(JT9_LINE.match, out.splitlines()) if m]
    cache.write_text(json.dumps(truth))
    return truth


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--capture", action="append", default=[], help="a dir of 6 kHz FT8 slots, scored by count")
    ap.add_argument("--ft4", help="a dir of 12 kHz FT4 periods, scored against jt9 -5")
    a = ap.parse_args()
    report.build_decoder()
    corpora = [("in_tree", report.wavs_12k(report.IN_TREE)), ("busy", report.wavs_12k(report.fetch_busy()))]
    captures = [(Path(d).name, sorted(Path(d).glob("*.wav"))) for d in a.capture]
    _, _, today8, starts8 = MODES["ft8"]
    jobs = [(name, files, b) for name, files in corpora + captures for b in starts8]
    with ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        results = list(pool.map(lambda j: decode(j[1], "ft8", j[2]), jobs))
    print("FT8: decode start, units two workers fit after it, then messages per set "
          "(corpora: of jt9's; captures: count)")
    for b in starts8:
        cells = []
        for (name, files, bb), found in zip(jobs, results):
            if bb != b:
                continue
            if name in ("in_tree", "busy"):
                hits = sum(len(set(found[str(w)]) & set(report.load_truth(name, w))) for w in files)
                total = sum(len(report.load_truth(name, w)) for w in files)
                cells.append(f"{name} {hits}/{total}")
            else:
                cells.append(f"{name} {sum(len(v) for v in found.values())}")
        mark = "  (today)" if b == today8 else ""
        print(f"  {b} blocks, {b * 0.16:.2f} s, {budget('ft8', b):5} units: " + ", ".join(cells) + mark)
    if a.ft4:
        truth = ft4_truth(a.ft4)
        files = sorted(truth)
        _, _, today4, starts4 = MODES["ft4"]
        with ThreadPoolExecutor(os.cpu_count() or 2) as pool:
            res4 = list(pool.map(lambda b: decode(files, "ft4", b), starts4))
        total = sum(len(set(v)) for v in truth.values())
        print("FT4 (a period that decodes alone): decode start, units, messages of jt9's")
        for b, found in zip(starts4, res4):
            hits = sum(len(found.get(f, set()) & set(truth[f])) for f in files)
            mark = "  (today)" if b == today4 else ""
            print(f"  {b} blocks, {b * 0.048:.2f} s, {budget('ft4', b):5} units: {hits}/{total}{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
