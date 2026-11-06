#!/usr/bin/env python3
"""Host test of the decode budget (docs/architecture/decode.md, The work budget).

On the in-tree corpus:
  * an ample budget decodes byte for byte what no budget does;
  * a tight budget decodes the same on every run, never counts past its budget, and lists a prefix of
    what no budget lists, in the same order: it stops the slot, it never reorders or changes it.

    python3 tools/decode_recall/test_budget.py
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402

TIMING = re.compile(r" (wall_ms|find_ms|decode_ms|find_us|decode_us)=\d+")
TIGHT = [3000, 4500, 6000]


def run(config, wavs):
    _decoded, _wall, log = report.run_decoder(config, wavs)
    return TIMING.sub("", log)


def per_file(log):
    """Per file: its message lines in order, and its END fields."""
    out = {}
    current = None
    for line in log.splitlines():
        if line.startswith("FILE "):
            current = line[5:]
            out[current] = ([], {})
        elif line.startswith("END ") and current is not None:
            out[current][1].update(kv.split("=") for kv in line[4:].split())
            current = None
        elif current is not None:
            out[current][0].append(line)
    return out


def main():
    report.build_decoder()
    wavs = report.wavs_12k(report.IN_TREE)
    fails = []

    unlimited = run(dict(report.TODAY, budget_units=0), wavs)
    ample = run(dict(report.TODAY, budget_units=1_000_000_000), wavs)
    if ample != unlimited:
        fails.append("an ample budget changed the output")
    full = per_file(unlimited)
    if any(fields.get("cut") != "0" for _lines, fields in full.values()):
        fails.append("no budget, yet a slot reads cut")

    for budget in TIGHT:
        config = dict(report.TODAY, budget_units=budget)
        first = run(config, wavs)
        if run(config, wavs) != first:
            fails.append(f"budget {budget}: two runs differ")
        cut = 0
        for path, (lines, fields) in per_file(first).items():
            if int(fields["units"]) > budget:
                fails.append(f"budget {budget}: {Path(path).name} counted {fields['units']} units")
            if lines != full[path][0][: len(lines)]:
                fails.append(f"budget {budget}: {Path(path).name} is not a prefix of the full decode")
            cut += fields["cut"] == "1"
        if cut == 0:
            fails.append(f"budget {budget}: cut no slot, so it tested nothing")

    for fail in fails:
        print("FAIL: " + fail)
    print(f"budget: {len(wavs)} slots, budgets {TIGHT}: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
