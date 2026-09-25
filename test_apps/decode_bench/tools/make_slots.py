#!/usr/bin/env python3
"""Prepare the corpus slots the decode benchmark embeds (docs/architecture/decode.md).

Writes data/slot_0.raw .. slot_4.raw (6 kHz mono int16, little-endian) and data/slots.txt. The busiest
slots are chosen on purpose: the benchmark measures worst-case decode time, and busy slots are where the
candidate cap binds. 12 kHz -> 6 kHz uses a windowed-sinc low-pass, not the firmware's stride decimator,
so the benchmark measures the decoder, not the aliasing question (RFC 0006 P3). The filter (127 taps, a
~500 Hz transition around 2.95 kHz) is good enough for timing and for host/device parity, which both see the
same samples; it is not the recall-grade decimator P1 needs.

The 20m_busy WAVs are fetched from howard0su/ft8_lib at a pinned SHA and never committed (RFC 0006 P1).

  python3 test_apps/decode_bench/tools/make_slots.py
"""

import math
import os
import struct
import subprocess
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
DATA = os.path.join(APP, "data")
CACHE = os.path.join(DATA, ".cache")
HOWARD_URL = "https://github.com/howard0su/ft8_lib.git"
HOWARD_SHA = "55ccf01"

IN_TREE = os.path.join(REPO, "components", "ft8_lib", "vendor", "test", "wav")
# Busiest slots by published truth count (checked 2026-09-23). Order is the slot index on the device.
SLOTS = [
    ("howard", "test/wav/20m_busy/test_21.wav"),   # 33 truth lines
    ("howard", "test/wav/20m_busy/test_19.wav"),   # 33
    ("howard", "test/wav/20m_busy/test_35.wav"),   # 32
    ("in_tree", "websdr_test6.wav"),                # 30
    ("in_tree", "websdr_test7.wav"),                # 27
]

TAPS = 127
CUTOFF_HZ = 2950.0


def fetch_howard():
    source = os.path.join(CACHE, "howard0su_ft8_lib")
    if not os.path.isdir(source):
        os.makedirs(CACHE, exist_ok=True)
        subprocess.run(["git", "clone", "-q", HOWARD_URL, source], check=True)
    subprocess.run(["git", "-C", source, "checkout", "-q", HOWARD_SHA], check=True)
    return source


def lowpass(rate):
    """Blackman-windowed sinc, unity DC gain."""
    fc = CUTOFF_HZ / rate
    mid = (TAPS - 1) / 2
    taps = []
    for n in range(TAPS):
        k = n - mid
        sinc = 2 * fc if k == 0 else math.sin(2 * math.pi * fc * k) / (math.pi * k)
        window = 0.42 - 0.5 * math.cos(2 * math.pi * n / (TAPS - 1)) + 0.08 * math.cos(4 * math.pi * n / (TAPS - 1))
        taps.append(sinc * window)
    total = sum(taps)
    return [t / total for t in taps]


def read_wav(path):
    with wave.open(path) as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit(f"{path}: expected 16-bit mono")
        rate = w.getframerate()
        frames = w.readframes(w.getnframes())
    return rate, list(struct.unpack(f"<{len(frames) // 2}h", frames))


def to_6k(path):
    rate, x = read_wav(path)
    if rate == 6000:
        return x
    if rate != 12000:
        raise SystemExit(f"{path}: expected 12 kHz, got {rate}")
    h = lowpass(rate)
    half = TAPS // 2
    padded = [0] * half + x + [0] * half
    out = []
    for i in range(0, len(x), 2):
        acc = 0.0
        for k, tap in enumerate(h):
            acc += tap * padded[i + k]
        out.append(max(-32768, min(32767, int(round(acc)))))
    return out


def main():
    os.makedirs(DATA, exist_ok=True)
    howard = fetch_howard()
    lines = []
    for index, (origin, name) in enumerate(SLOTS):
        path = os.path.join(howard, name) if origin == "howard" else os.path.join(IN_TREE, name)
        samples = to_6k(path)[:6000 * 15]
        with open(os.path.join(DATA, f"slot_{index}.raw"), "wb") as f:
            f.write(struct.pack(f"<{len(samples)}h", *samples))
        label = f"howard0su/ft8_lib@{HOWARD_SHA}:{name}" if origin == "howard" else f"ft8_lib/test/wav/{name}"
        lines.append(f"{index} {label}")
        print(f"slot_{index}.raw  {len(samples)} samples  {label}")
    with open(os.path.join(DATA, "slots.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
