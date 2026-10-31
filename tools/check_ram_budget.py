#!/usr/bin/env python3
"""Fails when the P4's static RAM has grown past its budget.

Static data and the heap share the P4's internal RAM, so every static byte is heap the running
station no longer has (docs/architecture/memory.md). The waterfall is static, so the budget no
longer guards one contiguous block; it guards the heap left for everything else, and it is a
ratchet: growth past it is a decision, made in memory.md, not a side effect. A host test cannot
see the ESP heap; this reads the firmware build instead.

Usage: tools/check_ram_budget.py poe_p4/build/poe_p4.elf   (needs the ESP-IDF environment)

BUDGET_BYTES is the only place the number lives.
"""
import subprocess
import sys

# Static RAM (.bss + .data in DRAM) the build may use: the build's own figure (348,372 bytes, once
# the waterfall was static and the C++ streams were gone), plus a margin. The ceiling is not this number. With the waterfall
# on the heap, the station ran at 272,332 bytes of statics (2026-10-24), and that waterfall's
# 161,076 bytes now sit in static RAM, so up to about 433,000 bytes the station keeps at least the
# heap it ran with then. Raise BUDGET_BYTES only by a decision recorded in memory.md.
BUDGET_BYTES = 352000

DRAM_SECTIONS = ('.dram0.data', '.dram0.bss', '.dram1.data', '.dram1.bss')


def static_ram(elf):
    out = subprocess.run(['riscv32-esp-elf-size', '-A', elf], check=True,
                         capture_output=True, text=True).stdout
    sizes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] in DRAM_SECTIONS:
            sizes[parts[0]] = int(parts[1])
    return sizes


def biggest_statics(elf, count=8):
    out = subprocess.run(['riscv32-esp-elf-nm', '-S', '--size-sort', '-C', elf], check=True,
                         capture_output=True, text=True).stdout
    rows = [l.split(None, 3) for l in out.splitlines() if len(l.split(None, 3)) == 4]
    rows = [r for r in rows if r[2] in 'bBdD']
    return [(int(r[1], 16), r[3]) for r in rows[-count:]][::-1]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sizes = static_ram(sys.argv[1])
    total = sum(sizes.values())
    print('static DRAM %d bytes (%s), budget %d' % (
        total, ', '.join('%s %d' % kv for kv in sorted(sizes.items())), BUDGET_BYTES))
    if total <= BUDGET_BYTES:
        return 0
    print('FAIL: static RAM is %d bytes over budget. Every static byte is heap the station '
          'loses; see docs/architecture/memory.md before raising BUDGET_BYTES.'
          % (total - BUDGET_BYTES))
    print('Largest statics:')
    for size, name in biggest_statics(sys.argv[1]):
        print('  %7d  %s' % (size, name))
    return 1


if __name__ == '__main__':
    sys.exit(main())
