#!/usr/bin/env python3
"""Score a recorded `/api/events` log from an operating session.

The log is one JSON event per line, as the sidekick serves them (docs/architecture/sidekick.md,
Port A), deduplicated by sequence number `s`: `d` 0 a log line, 1 a decode, 2 a queue entry,
4 the transmission's status, 7 the radio's state. Plain or gzipped.

    python3 tools/decode_bench/score_events.py LOG [--from HH:MM:SS] [--to HH:MM:SS]
                                               [--call KB2SLO] [--timeline CALL]

Prints, within the window (UTC, on the log's first day):
  * decode load per mode: slots decoded, messages, `left`, slots stopped at their budget;
  * each decode that ran past its boundary and each `STALL`, with the nearest transmission start;
  * each report we sent beside the SNR of the reply it answered;
  * each R-report's audio offset, per QSO;
  * the longest wait between two transmissions to the same station.
With --timeline, every decode from or to CALL and every transmission to it, in order.
"""

import argparse
import gzip
import json
import re
import statistics
import sys
import time

SLOT_RE = re.compile(r"SLOT(/FT4)? \S+ c\d+ m(\d+) t\d+ dec\d+ left(-?\d+)")
STALL_RE = re.compile(r"STALL (\d+) gap(\d+) empty(\d+) at(\d+)")
FT8_SLOT_S = 15  # a STALL names its FT8 slot by index: the slot starts at index * 15 s
REPORT_RE = re.compile(r"^(\S+) (\S+) (R?)([+-]\d+)$")


def hms(t):
    return time.strftime("%H:%M:%S", time.gmtime(t))


def load(path):
    opener = gzip.open if path.endswith(".gz") else open
    rows = {}
    with opener(path, "rt") as f:
        for line in f:
            e = json.loads(line)
            rows.setdefault(e["s"], e)
    return [rows[s] for s in sorted(rows)]


def day_time(events, text):
    day = min(e["t"] for e in events if e.get("t")) // 86400 * 86400
    h, m, s = (int(x) for x in text.split(":"))
    return day + h * 3600 + m * 60 + s


def tx_starts(events):
    """(time, message, offset) of each transmission, from the status rows' rising edge."""
    out, active = [], False
    for e in events:
        if e["d"] != 4:
            continue
        if e["active"] and not active:
            out.append((e["t"], e["x"], e["hz"]))
        active = bool(e["active"])
    return out


def nearest(t, starts):
    return min((s[0] - t for s in starts), key=abs) if starts else None


def decode_load(events, starts):
    per = {"FT4": [], "FT8": []}
    late, stalls = [], []
    for e in events:
        if e["d"] != 0:
            continue
        m = SLOT_RE.match(e["x"])
        if m:
            mode = "FT4" if m.group(1) else "FT8"
            per[mode].append((int(m.group(2)), int(m.group(3)), "!" in e["x"]))
            if int(m.group(3)) < 0:
                late.append((e["t"], mode, int(m.group(3))))
        m = STALL_RE.match(e["x"])
        if m:  # when the wait ended: the slot's start plus `at`, not when the line was sent
            ended = int(m.group(1)) * FT8_SLOT_S + int(m.group(4)) / 1000
            stalls.append((ended, int(m.group(2)), int(m.group(3))))
    print("== Decode load")
    for mode, rows in per.items():
        if not rows:
            continue
        msgs = [r[0] for r in rows]
        left = [r[1] for r in rows]
        print(f"{mode}: {len(rows)} decoded, messages {min(msgs)}-{max(msgs)} mean {statistics.mean(msgs):.1f}, "
              f"left min {min(left)} median {statistics.median(left):g} ms, "
              f"{sum(r[2] for r in rows)} at their budget")
    print(f"Transmissions: {len(starts)}")
    print("== Decodes past their boundary (nearest transmission start, s)")
    for t, mode, left in late:
        print(f"{hms(t)} {mode} left {left} ms, {nearest(t, starts):+d}")
    print("== STALL, when the wait ended (gap ms, empty reads, nearest transmission start, s)")
    for t, gap, empty in stalls:
        print(f"{hms(t)}.{int(t * 10) % 10} gap {gap} empty {empty}, {nearest(t, starts):+.1f}")
    if stalls:
        gaps = [s[1] for s in stalls]
        print(f"{len(stalls)} STALL, {min(gaps)}-{max(gaps)} ms, {sum(g < 75 for g in gaps)} under 75 ms")


def reports(events, starts, mycall):
    """Each report we sent, beside the SNR of the newest decode from that station to us before it."""
    print("== Reports sent against the reply's SNR")
    sent = set()
    for t, msg, hz in starts:
        m = REPORT_RE.match(msg)
        if not m or m.group(2) != mycall or (m.group(1), msg) in sent:
            continue
        sent.add((m.group(1), msg))
        dx = m.group(1)
        heard = [e for e in events if e["d"] == 1 and e["t"] <= t and e["x"].split()[:2] == [mycall, dx]]
        reply = f"{heard[-1]['snr']:+d} dB ({heard[-1]['x']})" if heard else "none decoded"
        print(f"{hms(t)} to {dx}: {m.group(3)}{m.group(4)}, their reply {reply}")


def offsets(starts, mycall):
    print("== Audio offset of each R-report, per station")
    per = {}
    for t, msg, hz in starts:
        m = REPORT_RE.match(msg)
        if m and m.group(2) == mycall and m.group(3) == "R":
            per.setdefault(m.group(1), []).append((t, hz))
    for dx, rows in per.items():
        print(f"{dx}: " + ", ".join(f"{hms(t)} {hz} Hz" for t, hz in rows))


def entries(events):
    """Each queue entry's (eid, call, first seen, removed), from its QUEUE_ENTRY rows; state 6 removes it."""
    out = {}
    for e in events:
        if e["d"] != 2:
            continue
        rec = out.setdefault(e["eid"], [e["eid"], e["x"], e["t"], None])
        if e["x"]:
            rec[1] = e["x"]
        if e["st"] == 6 and rec[3] is None:
            rec[3] = e["t"]
    return list(out.values())


def waits(events, starts, mycall, min_s=60):
    """Waits within one queue entry: a station tapped again is a new entry, so its gap is not counted."""
    print(f"== Waits over {min_s} s between transmissions within one queue entry")
    for eid, call, first, removed in entries(events):
        end = removed if removed is not None else float("inf")
        times = [(t, msg) for t, msg, hz in starts
                 if first <= t <= end and msg.split()[:2] == [call, mycall]]
        for (a, _), (b, msg) in zip(times, times[1:]):
            if b - a > min_s:
                print(f"{call} (entry {eid}): {b - a} s, {hms(a)} to {hms(b)} ({msg})")


def timeline(events, call):
    print(f"== Timeline for {call}")
    active = False
    for e in events:
        if e["d"] == 1 and call in e["x"].split():
            print(f"{hms(e['t'])} decode {e['x']} {e['snr']:+d} dB {e['hz']} Hz")
        elif e["d"] == 4:
            if e["active"] and not active and e["x"].split()[:1] == [call]:
                print(f"{hms(e['t'])} TX {e['x']} {e['hz']} Hz")
            active = bool(e["active"])
        elif e["d"] == 0 and e["x"].startswith(("SKIP ", "SLOT ")):
            print(f"{hms(e['t'])} FT8 {e['x'].split()[0].lower()} {e['x'].split()[1]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("--from", dest="start", help="UTC HH:MM:SS")
    ap.add_argument("--to", dest="end", help="UTC HH:MM:SS, exclusive")
    ap.add_argument("--call", default="KB2SLO", help="our call")
    ap.add_argument("--timeline", metavar="CALL")
    args = ap.parse_args()

    events = load(args.log)
    t0 = day_time(events, args.start) if args.start else 0
    t1 = day_time(events, args.end) if args.end else float("inf")
    events = [e for e in events if t0 <= e.get("t", 0) < t1]
    if not events:
        sys.exit("no events in the window")
    print(f"Window {hms(events[0]['t'])}-{hms(events[-1]['t'])} UTC, {len(events)} events")
    if args.timeline:
        timeline(events, args.timeline)
        return
    starts = tx_starts(events)
    decode_load(events, starts)
    reports(events, starts, args.call)
    offsets(starts, args.call)
    waits(events, starts, args.call)


if __name__ == "__main__":
    main()
