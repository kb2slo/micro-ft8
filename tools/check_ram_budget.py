#!/usr/bin/env python3
"""Fails when the P4's static RAM has grown past what the decode pipeline can live with.

The pipeline allocates its waterfall (161076 bytes) as one block from internal RAM when the
radio's audio starts, and gives up with `Waterfall alloc failed` when no block is that big. Every
static byte comes out of that RAM, so a few KB of new static buffers silently stop all decoding
(2026-10-24: a10918e and b433403 added 14.6 KB of statics, and no slot decoded). A host test
cannot see the ESP heap; this reads the firmware build instead.

Usage: tools/check_ram_budget.py poe_p4/build/poe_p4.elf   (needs the ESP-IDF environment)

BUDGET_BYTES is the only place the number lives. When a change needs more, allocate on first use
after the waterfall (see decode_pipeline.cpp, `alloc_slot_result()`) rather than raising it, and
re-derive it from the `AUDIO_PIPE_BEFORE_MONITOR_INIT` line: the budget is the static RAM at
which that line's `largest` just covers WATERFALL_BYTES, less a margin.
"""
import subprocess
import sys

WATERFALL_BYTES = 161076
# Static RAM (.bss + .data in DRAM) the build may use. Measured 2026-10-24 on the board at
# 272,332 bytes: the pipeline's start logged `largest=163840` against the 161,076 it needs, so
# 2,764 bytes of growth stops all decoding. The budget leaves about 1.7 KB of that and fails
# before the board does.
BUDGET_BYTES = 274000

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
    print('FAIL: static RAM is %d bytes over budget. The waterfall needs one free block of %d '
          'bytes; static data comes out of it.' % (total - BUDGET_BYTES, WATERFALL_BYTES))
    print('Largest statics:')
    for size, name in biggest_statics(sys.argv[1]):
        print('  %7d  %s' % (size, name))
    return 1


if __name__ == '__main__':
    sys.exit(main())
