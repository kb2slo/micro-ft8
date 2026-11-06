#!/usr/bin/env python3
"""Score a capture beside the P4: what the decode budget costs on the air, and the P4 against the host.

SLOT_DIR is tools/hl2_pcap.py's output (one 48 kHz I/Q WAV per slot and manifest.jsonl). Each slot goes
through the I/Q front end at the P4's 6 kHz (tools/iq_convert, the HL2's channel: 1,500 Hz, Q negated,
as components/hl2_proto/include/hl2_rx_frontend.h configures it), then tools/decode_recall decodes it with
no budget and with the firmware's (decode_limits.h). It prints, per slot and in total, the messages each
decoded, and every message the budget cost, with its SNR: on identical input, so the difference is the
budget's alone (docs/architecture/decode.md, **The work budget**).

With --p4 EVENTS.jsonl (decode events as GET /api/events returns them, one per line), it also prints, per
slot, what the P4 itself decoded against the host's replay at the firmware's budget, matched by slot index.

  python3 tools/decode_bench/capture_ab.py SLOT_DIR [--p4 events.jsonl] [--jobs 4]

Needs tools/iq_convert and tools/decode_recall built (`make -C`). Conversions go in SLOT_DIR/conv6k_n24.
"""
import argparse
import calendar
import collections
import json
import os
import re
import subprocess
import sys
import time
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONV = os.path.join(ROOT, "tools", "iq_convert", "iq_convert")
DECODER = os.path.join(ROOT, "tools", "decode_recall", "decode_recall")
# The HL2's samples arrive a few bits deep in 24, so a 16-bit WAV at their level leaves the band noise under
# one LSB (an RMS of 0.9 on a quiet 12 m slot); the P4 decodes floats. Scaled to a set RMS first, as
# iq_convert's --normalize-db does, the decoder's per-block AGC sees what it does on the P4.
HL2_CHANNEL = ["--center-hz", "1500", "--negate-q", "--output-rate", "6000", "--normalize-db", "-24"]
MSG = re.compile(r"^\S+\s+(-?\d+)\s+\S+\s+\S+\s+~\s+(.*)$")


def slot_index(name):
    """The 15 s slot index (epoch / 15) a YYMMDD_HHMMSS.wav file is named for."""
    return calendar.timegm(time.strptime(os.path.basename(name)[:13], "%y%m%d_%H%M%S")) // 15


def convert(job):
    src, dst = job
    clipped = 0
    if not os.path.exists(dst):
        out = subprocess.run([CONV, *HL2_CHANNEL, "-o", dst + ".part", src], check=True, capture_output=True,
                             text=True).stdout
        m = re.search(r"clipped=(\d+)", out)
        clipped = int(m.group(1)) if m else 0
        os.replace(dst + ".part", dst)
    return dst, clipped


def decode(wavs, budget):
    """{slot index: {message text: SNR}} and {slot index: END fields} for one budget (None: firmware's)."""
    cmd = [DECODER] + ([] if budget is None else ["--budget-units", str(budget)]) + wavs
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    msgs, ends, cur = {}, {}, None
    for line in out.splitlines():
        if line.startswith("FILE "):
            cur = slot_index(line[5:].strip())
            msgs[cur] = {}
        elif line.startswith("END ") and cur is not None:
            ends[cur] = dict(kv.split("=") for kv in line[4:].split())
            cur = None
        elif cur is not None:
            m = MSG.match(line.strip())
            if m:
                msgs[cur][m.group(2).strip()] = int(m.group(1))
    return msgs, ends


def load_p4(path):
    """{slot index: set of message texts} from decode events (d 1), by their own slot index."""
    out = collections.defaultdict(set)
    with open(path) as f:
        for line in f:
            e = json.loads(line)
            if e.get("d") == 1 and "si" in e and e.get("x"):
                out[e["si"]].add(e["x"].strip())
    return out


def hashed(text):
    """A message with every hashed call shown as <...>, as a fresh decode shows a call it has not heard."""
    return re.sub(r"<[^>]*>", "<...>", text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("slot_dir")
    ap.add_argument("--p4", help="the P4's decode events for the same hour, one JSON object a line")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--budget-units", type=int, default=None,
                    help="the budgeted arm's budget; the firmware's (decode_limits.h) by default")
    args = ap.parse_args()

    with open(os.path.join(args.slot_dir, "manifest.jsonl")) as f:
        slots = [json.loads(line) for line in f]
    conv = os.path.join(args.slot_dir, "conv6k_n24")
    os.makedirs(conv, exist_ok=True)
    jobs = [(os.path.join(args.slot_dir, s["file"]), os.path.join(conv, s["file"])) for s in slots]
    with Pool(args.jobs) as pool:
        done = pool.map(convert, jobs)
    wavs = [d for d, _ in done]
    clipped = sum(c for _, c in done)
    lost = sum(s.get("lost_packets", 0) for s in slots)
    offsets = sorted({s.get("start_offset_s", 0.0) for s in slots})

    free, _ = decode(wavs, 0)
    fw, ends = decode(wavs, args.budget_units)
    cut = sum(e.get("cut") == "1" for e in ends.values())
    print(f"{len(wavs)} slots, start offset {offsets} s, packets lost {lost}, samples clipped converting {clipped}; "
          f"the firmware's budget cut {cut}")
    cost = []
    for si in sorted(free):
        for text, snr in free[si].items():
            if text not in fw.get(si, {}):
                cost.append((si, snr, text))
    gained = sum(len(set(fw[si]) - set(free.get(si, {}))) for si in fw)
    n_free = sum(len(v) for v in free.values())
    n_fw = sum(len(v) for v in fw.values())
    which = "firmware's budget" if args.budget_units is None else f"budget {args.budget_units}"
    print(f"host, no budget: {n_free} messages; {which}: {n_fw} ({len(cost)} lost, {gained} gained)")
    for si, snr, text in cost:
        print(f"  lost to the budget: slot {si} {snr:+d} dB  {text}")

    if args.p4:
        p4 = load_p4(args.p4)
        both = [si for si in sorted(fw) if si in p4]
        p4_only = host_only = same = 0
        for si in both:
            a = {hashed(t) for t in p4[si]}
            snr = {hashed(t): v for t, v in fw[si].items()}
            b = set(snr)
            same += len(a & b)
            p4_only += len(a - b)
            host_only += len(b - a)
            for text in sorted(a - b):
                print(f"  P4 only: slot {si}  {text}")
            for text in sorted(b - a):
                print(f"  host only: slot {si} {snr[text]:+d} dB  {text}")
        print(f"P4 against the host at the firmware's budget, {len(both)} slots: both {same}, "
              f"P4 only {p4_only}, host only {host_only}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
