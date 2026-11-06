#!/usr/bin/env python3
"""The decode budget against recall (docs/architecture/decode.md, The work budget).

Decodes both corpora under each budget in units, at the firmware's costs per search and per candidate
(decode_limits.h), and prints the jt9 messages found and how many slots the budget cut. With --units,
also prints each slot's work counted with no budget: searches, candidates tried, LDPC iterations.

    python3 tools/decode_recall/budget.py
    python3 tools/decode_recall/budget.py --budgets 7000 6000 5000 --units

The counts are the host's and the P4's alike; host time is not device time.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report  # noqa: E402

DEFAULT_BUDGETS = [0, 7500, 7000, 6500, 6000, 5500, 5000, 4500]


def counts(log):
    """Per file: the END line's fields, as ints."""
    out = {}
    current = None
    for line in log.splitlines():
        if line.startswith("FILE "):
            current = line[5:]
        elif line.startswith("END ") and current is not None:
            out[current] = {k: int(v) for k, v in (kv.split("=") for kv in line[4:].split())}
            current = None
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--budgets", type=int, nargs="+", default=DEFAULT_BUDGETS)
    parser.add_argument("--units", action="store_true", help="print each slot's work with no budget")
    args = parser.parse_args()

    report.build_decoder()
    corpora = [("in_tree", report.wavs_12k(report.IN_TREE)), ("busy", report.wavs_12k(report.fetch_busy()))]
    slots = sum(len(wavs) for _, wavs in corpora)
    print(f"cost_search={report.TODAY['cost_search']} cost_candidate={report.TODAY['cost_candidate']} "
          f"firmware budget_units={report.TODAY['budget_units']}")
    for budget in args.budgets:
        config = dict(report.TODAY, budget_units=budget)
        row = []
        cut = 0
        for name, wavs in corpora:
            decoded, _wall, log = report.run_decoder(config, wavs)
            cut += len(re.findall(r"\bcut=1\b", log))
            score = report.score_corpus(name, wavs, decoded)
            row.append(f"{name} {score['hits']}/{score['truth']}")
        label = "none" if budget == 0 else str(budget)
        print(f"budget {label:>5}: {'  '.join(row)}  slots cut {cut}/{slots}")

    if args.units:
        config = dict(report.TODAY, budget_units=0)
        print("corpus\tfile\tdecoded\tsearches\ttried\titerations\tunits")
        for name, wavs in corpora:
            _decoded, _wall, log = report.run_decoder(config, wavs)
            for path, c in counts(log).items():
                print(f"{name}\t{Path(path).name}\t{c['dec']}\t{c['searches']}\t{c['tried']}\t{c['iters']}\t{c['units']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
