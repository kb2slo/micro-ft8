#!/usr/bin/env python3
"""Score FT8 decoders against jt9 on a directory of 12 kHz WAVs.

Throwaway comparison used for RFC 0006 section 4.1; B25 replaces it with the real
harness. Truth is `jt9 -8 -d DEPTH`; decodes are matched on normalised message text.

  python3 tools/decode_bench/compare_decoders.py WAV_DIR NAME=DECODER_BINARY [...]

Each DECODER_BINARY takes one WAV path and prints WSJT-X-style lines
(`HHMMSS SNR DT FREQ ~ MESSAGE`), as ft8_lib's demo/decode_ft8 does. Build one with
`make decode_ft8` in a *copy* of the ft8_lib tree (the submodule, or a clone of
howard0su/ft8_lib). On current Apple clang, kgoba's demo/decode_ft8.c needs
`#include <stdio.h>` added before it compiles; howard0su's builds as-is.
"""

import argparse
import os
import re
import subprocess
import tempfile
import time
import wave

LINE = re.compile(r"^\s*\d{4,6}\s+[-+]?\d+(?:\.\d+)?\s+[-+]?\d+\.\d+\s+\d+\s+~\s+(.*?)\s*$")


def messages(stdout):
    out = set()
    for line in stdout.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        text = re.sub(r"<[^>]*>", "<...>", m.group(1))  # hashed calls
        text = re.sub(r"\s+a\d$", "", text)             # jt9 AP-pass marker
        out.add(" ".join(text.split()))
    return out


def wavs_12k(wav_dir):
    for name in sorted(os.listdir(wav_dir)):
        path = os.path.abspath(os.path.join(wav_dir, name))
        if not name.endswith(".wav"):
            continue
        with wave.open(path) as w:
            if w.getframerate() == 12000:
                yield path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav_dir")
    ap.add_argument("decoders", nargs="+", help="NAME=path/to/binary")
    ap.add_argument("--depth", type=int, default=3, help="jt9 decode depth (default 3)")
    args = ap.parse_args()

    decoders = [(n, os.path.abspath(e)) for n, e in (d.split("=", 1) for d in args.decoders)]
    files = list(wavs_12k(args.wav_dir))
    scratch = tempfile.mkdtemp(prefix="jt9_")  # jt9 writes wisdom/timer files to its cwd
    truth_total = 0
    stats = {name: [0, 0, 0.0] for name, _ in decoders}  # matched, extras, seconds

    for path in files:
        truth = messages(subprocess.run(["jt9", "-8", "-d", str(args.depth), path],
                                        capture_output=True, text=True, cwd=scratch).stdout)
        truth_total += len(truth)
        for name, exe in decoders:
            t0 = time.perf_counter()
            got = messages(subprocess.run([exe, path], capture_output=True, text=True).stdout)
            stats[name][2] += time.perf_counter() - t0
            stats[name][0] += len(got & truth)
            stats[name][1] += len(got - truth)

    if truth_total == 0:
        raise SystemExit("jt9 produced no decodes -- is jt9 on PATH and are these 12 kHz FT8 WAVs?")
    print(f"{len(files)} files, jt9 -d {args.depth}: {truth_total} unique messages")
    for name, (hit, extra, secs) in stats.items():
        print(f"{name:12s} recall {100.0 * hit / truth_total:5.1f}% ({hit}/{truth_total})"
              f"  extras {extra}  {1000.0 * secs / len(files):6.0f} ms/slot wall")


if __name__ == "__main__":
    main()
