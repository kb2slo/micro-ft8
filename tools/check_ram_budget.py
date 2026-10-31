#!/usr/bin/env python3
"""Fails when the P4's static RAM has grown past its budget.

Static data and the heap share the P4's internal RAM, so every static byte is heap the running
station no longer has (docs/architecture/memory.md). The waterfall and decode working sets live
in PSRAM (`.ext_ram.bss`); this script budgets internal DRAM only. It is a ratchet: growth past
the budget is a decision, made in memory.md, not a side effect. A host test cannot see the ESP
heap; this reads the firmware build instead.

Usage: tools/check_ram_budget.py poe_p4/build/poe_p4.elf   (needs the ESP-IDF environment)

BUDGET_BYTES is the only place the number lives.
"""
import subprocess
import sys

# Internal static RAM (.bss + .data in DRAM) the build may use. After row tfyy moved the
# capture path to PSRAM too, the build sat at about 61,000 bytes (2026-10-31); 85,000 keeps
# the same ~24 KB margin for small internal statics. Raise BUDGET_BYTES only by a decision
# recorded in memory.md.
BUDGET_BYTES = 85000

DRAM_SECTIONS = ('.dram0.data', '.dram0.bss', '.dram1.data', '.dram1.bss')
EXT_RAM_BSS = '.ext_ram.bss'


def section_sizes(elf):
    out = subprocess.run(['riscv32-esp-elf-size', '-A', elf], check=True,
                         capture_output=True, text=True).stdout
    sizes = {}
    vmas = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0].startswith('.'):
            try:
                sizes[parts[0]] = int(parts[1])
                vmas[parts[0]] = int(parts[2])
            except ValueError:
                pass
    return sizes, vmas


def static_ram(sizes):
    return {s: sizes.get(s, 0) for s in DRAM_SECTIONS}


def biggest_dram_statics(elf, vmas, count=15):
    """Largest internal-DRAM statics. PSRAM (.ext_ram.bss) symbols are omitted."""
    out = subprocess.run(['riscv32-esp-elf-nm', '-S', '--size-sort', '-C', elf], check=True,
                         capture_output=True, text=True).stdout
    sizes_map, _ = section_sizes(elf)
    ext_end = None
    if EXT_RAM_BSS in sizes_map and EXT_RAM_BSS in vmas:
        ext_end = vmas[EXT_RAM_BSS] + sizes_map[EXT_RAM_BSS]

    rows = []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) != 4 or parts[2] not in 'bBdD':
            continue
        addr = int(parts[0], 16)
        size = int(parts[1], 16)
        if ext_end is not None and vmas[EXT_RAM_BSS] <= addr < ext_end:
            continue
        rows.append((size, parts[3]))
    rows.sort(reverse=True)
    return rows[:count]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    elf = sys.argv[1]
    sizes, vmas = section_sizes(elf)
    dram = static_ram(sizes)
    total = sum(dram.values())
    print('static DRAM %d bytes (%s), budget %d' % (
        total, ', '.join('%s %d' % kv for kv in sorted(dram.items())), BUDGET_BYTES))
    if EXT_RAM_BSS in sizes:
        print('static PSRAM %s %d bytes (not in budget)' % (EXT_RAM_BSS, sizes[EXT_RAM_BSS]))
    # Every build, not only a failing one: an unused buffer passes the budget and shows only here.
    print('Largest internal statics:')
    for size, name in biggest_dram_statics(elf, vmas):
        print('  %7d  %s' % (size, name))
    if total <= BUDGET_BYTES:
        return 0
    print('FAIL: static RAM is %d bytes over budget. Every static byte is heap the station '
          'loses; see docs/architecture/memory.md before raising BUDGET_BYTES.'
          % (total - BUDGET_BYTES))
    return 1


if __name__ == '__main__':
    sys.exit(main())
