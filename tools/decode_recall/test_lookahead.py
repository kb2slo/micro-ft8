#!/usr/bin/env python3
"""Host test of decodes run ahead of their commits (decode_core.h; docs/architecture/decode.md, Channels).

On both corpora, with no budget and with the firmware's, decoding each candidate 1, 2 or 3 candidates ahead
of its commit, or every candidate at the start of its search, lists byte for byte what the serial order
lists and counts the same work. Also prints what running ahead cost: decodes run again at commit, and
decodes thrown away, as a share of the candidates committed.

    python3 tools/decode_recall/test_lookahead.py
"""

import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402

# Wall-clock fields differ run to run; redone and wasted are what running ahead changes.
VARIES = re.compile(r" (wall_ms|find_ms|decode_ms|find_us|decode_us|redone|wasted)=\d+")
COST = re.compile(r"tried=(\d+).* redone=(\d+) wasted=(\d+)")
PASS = 256  # BENCH_LOOKAHEAD_PASS
LOOKAHEADS = [1, 2, 3, PASS]


def run(config, wavs):
    _decoded, _wall, log = report.run_decoder(config, wavs)
    return log


def cost(log):
    tried = redone = wasted = 0
    for match in COST.finditer(log):
        tried += int(match.group(1))
        redone += int(match.group(2))
        wasted += int(match.group(3))
    return tried, redone, wasted


def main():
    report.build_decoder()
    corpora = [("in_tree", report.wavs_12k(report.IN_TREE)), ("busy", report.wavs_12k(report.fetch_busy()))]
    fails = []
    jobs = [(name, wavs, budget, lookahead) for name, wavs in corpora
            for budget in (0, report.TODAY["budget_units"]) for lookahead in [0] + LOOKAHEADS]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        logs = list(pool.map(lambda j: run(dict(report.TODAY, budget_units=j[2], lookahead=j[3]), j[1]), jobs))
    by_job = {(name, budget, lookahead): log for (name, _w, budget, lookahead), log in zip(jobs, logs)}
    for name, _wavs, budget, lookahead in jobs:
        log = by_job[(name, budget, lookahead)]
        if lookahead == 0:
            if cost(log)[1:] != (0, 0):
                fails.append(f"{name} budget {budget}: the serial order redid or wasted a decode")
            continue
        label = "pass" if lookahead == PASS else str(lookahead)
        if VARIES.sub("", log) != VARIES.sub("", by_job[(name, budget, 0)]):
            fails.append(f"{name} budget {budget} lookahead {label}: output differs from serial")
        tried, redone, wasted = cost(log)
        print(f"{name:8} budget {budget:5} lookahead {label:>4}: {tried} committed, "
              f"{redone} redone ({100 * redone / tried:.1f}%), {wasted} wasted ({100 * wasted / tried:.1f}%)")
    for fail in fails:
        print("FAIL: " + fail)
    print("lookahead: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
