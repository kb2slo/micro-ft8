#!/usr/bin/env python3
"""Host test of decodes run ahead of their commits (decode_core.h, decode_workers.h; docs/architecture/decode.md,
Channels).

On both corpora, with no budget and with the firmware's, decoding each candidate 1, 2 or 3 candidates ahead
of its commit, or every candidate at the start of its search, lists byte for byte what the serial order
lists and counts the same work; and so does the firmware's driver with its helper thread (two workers),
run several times, since each run's timing differs. And a mix of channels: the in-tree FT8 slots interleaved
with synthetic FT4 periods (decode_recall --synth-ft4) in one run, as the P4's two channels share one decoder
and helper, list each file byte for byte as its mode alone does, with one worker or two. Also prints what running ahead cost: decodes run again
at commit, and decodes thrown away, as a share of the candidates committed.

    python3 tools/decode_recall/test_lookahead.py
"""

import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402

# Wall-clock fields differ run to run; redone and wasted are what running ahead changes.
VARIES = re.compile(r" (wall_ms|find_ms|decode_ms|find_us|decode_us|redone|wasted)=\d+")
COST = re.compile(r"tried=(\d+).* redone=(\d+) wasted=(\d+)")
PASS = 256  # BENCH_LOOKAHEAD_PASS
LOOKAHEADS = [1, 2, 3, PASS]
THREADED_RUNS = 3  # two-worker runs per corpus and budget


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


def per_file_lines(log):
    """Per FILE: its message and END lines, timings and run-ahead counts removed."""
    out, current = {}, None
    for line in VARIES.sub("", log).splitlines():
        if line.startswith("FILE "):
            current = line[5:]
            out[current] = []
        elif current is not None and (line.startswith("000000") or line.startswith("END ")):
            out[current].append(line)
    return out


def mixed_channels():
    fails = []
    ft8 = [str(p) for p in report.wavs_12k(report.IN_TREE)][:12]
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(report.DECODER), "--synth-ft4", tmp], check=True, capture_output=True)
        ft4 = ["ft4:" + os.path.join(tmp, n) for n in sorted(os.listdir(tmp))]
        mix = [f for pair in zip(ft8, ft4 * 3) for f in pair]

        def decode(files, workers):
            out = subprocess.run([str(report.DECODER), "--workers", str(workers)] + files,
                                 capture_output=True, text=True, check=True).stdout
            return per_file_lines(out)

        alone = dict(decode(ft8, 1), **decode(ft4, 1))
        if sum(1 for lines in alone.values() for l in lines if l.startswith("000000")) == 0:
            fails.append("mix: nothing decoded, so it tested nothing")
        runs = [decode(mix, 1)] + [decode(mix, 2) for _ in range(THREADED_RUNS)]
        for k, run in enumerate(runs):
            label = "one worker" if k == 0 else f"two workers #{k}"
            for f in mix:
                if run.get(f) != alone.get(f):
                    fails.append(f"mix, {label}: {os.path.basename(f)} differs from its mode alone")
                    break
        n4 = sum(1 for f in ft4 for l in alone[f] if l.startswith("000000"))
        print(f"mix: {len(ft8)} FT8 slots and {len(ft4)} FT4 periods interleaved, {n4} FT4 messages; "
              f"one and two workers list each file as its mode alone")
    return fails


def main():
    report.build_decoder()
    corpora = [("in_tree", report.wavs_12k(report.IN_TREE)), ("busy", report.wavs_12k(report.fetch_busy()))]
    fails = []
    jobs = [(name, wavs, budget, lookahead) for name, wavs in corpora
            for budget in (0, report.TODAY["budget_units"])
            for lookahead in [0] + LOOKAHEADS + [-r for r in range(1, THREADED_RUNS + 1)]]

    def config(job):
        lookahead = job[3]
        if lookahead < 0:  # a two-worker run
            return dict(report.TODAY, budget_units=job[2], lookahead=0, workers=2)
        return dict(report.TODAY, budget_units=job[2], lookahead=lookahead)

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        logs = list(pool.map(lambda j: run(config(j), j[1]), jobs))
    by_job = {(name, budget, lookahead): log for (name, _w, budget, lookahead), log in zip(jobs, logs)}
    for name, _wavs, budget, lookahead in jobs:
        log = by_job[(name, budget, lookahead)]
        if lookahead == 0:
            if cost(log)[1:] != (0, 0):
                fails.append(f"{name} budget {budget}: the serial order redid or wasted a decode")
            continue
        label = "pass" if lookahead == PASS else f"2 workers #{-lookahead}" if lookahead < 0 else str(lookahead)
        if VARIES.sub("", log) != VARIES.sub("", by_job[(name, budget, 0)]):
            fails.append(f"{name} budget {budget} lookahead {label}: output differs from serial")
        tried, redone, wasted = cost(log)
        print(f"{name:8} budget {budget:5} lookahead {label:>12}: {tried} committed, "
              f"{redone} redone ({100 * redone / tried:.1f}%), {wasted} wasted ({100 * wasted / tried:.1f}%)")
    fails += mixed_channels()
    for fail in fails:
        print("FAIL: " + fail)
    print("lookahead: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
