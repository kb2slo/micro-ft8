#!/usr/bin/env python3
"""Fit the decode's time per unit, with one worker and with two, from a decode_bench log.

The "today", "full" and "full3" rows are one worker; "today_2w", "full_2w" and "full3_2w" the same with the
second (bench_configs.h). Per worker count, a least-squares line of total_ms (search plus decode) on units,
and from it the time a slot of 6,000 and of 10,800 units would take: the budget today, and the 1.8 times
two workers were asked to fit in the 2.36 s guard (docs/architecture/decode.md, The work budget). Also checks that
each two-worker row printed exactly the messages its one-worker row did.

    python3 test_apps/decode_bench/tools/fit_units.py p4_bench.log
"""

import re
import sys

ONE = ["today", "full", "full3"]
TWO = [name + "_2w" for name in ONE]
GUARD_MS = 2360
UNITS = [6000, 10800]
RESULT = re.compile(r"^RESULT (\S+) (\d+) ok=1 .*total_ms=([\d.]+) .*units=(\d+)")


def fit(points):
    n = len(points)
    mx = sum(u for u, _ in points) / n
    my = sum(t for _, t in points) / n
    sxx = sum((u - mx) ** 2 for u, _ in points)
    slope = sum((u - mx) * (t - my) for u, t in points) / sxx
    intercept = my - slope * mx
    worst = max(abs(t - (intercept + slope * u)) for u, t in points)
    return intercept, slope, worst


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    results = {}
    messages = {}
    for line in open(sys.argv[1], errors="replace"):
        m = RESULT.match(line)
        if m:
            results.setdefault(m.group(1), []).append((int(m.group(4)), float(m.group(3))))
        elif line.startswith("MSG "):
            parts = line.split(" ", 3)
            messages.setdefault(parts[1], []).append(parts[2] + " " + parts[3].rstrip("\n"))
    failed = 0
    for one, two in zip(ONE, TWO):
        if messages.get(one) != messages.get(two):
            print(f"FAIL: {two} printed other messages than {one}")
            failed = 1
    lines = {}
    for label, names in (("1 worker", ONE), ("2 workers", TWO)):
        points = [p for name in names for p in results.get(name, [])]
        if len(points) < 3:
            print(f"{label}: too few RESULT lines")
            return 1
        lines[label] = fit(points)
        a, b, worst = lines[label]
        span = f"{min(u for u, _ in points)}-{max(u for u, _ in points)} units"
        projected = ", ".join(f"{u} units {a + b * u:.0f} ms" for u in UNITS)
        print(f"{label}: {a:.0f} ms + {b * 1000:.1f} us a unit (worst residual {worst:.0f} ms, {len(points)} slots, "
              f"{span}); {projected}")
    a1, b1, _ = lines["1 worker"]
    a2, b2, _ = lines["2 workers"]
    print(f"a unit costs {b1 / b2:.2f} times less with two workers")
    fits = (GUARD_MS - a2) / b2
    print(f"in the {GUARD_MS} ms guard two workers fit {fits:.0f} units, {fits / 6000:.2f} times 6000")
    return failed


if __name__ == "__main__":
    sys.exit(main())
