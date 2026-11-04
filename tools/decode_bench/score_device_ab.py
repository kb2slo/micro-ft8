#!/usr/bin/env python3
"""Score an on-device IQ/audio A/B (Station qmx_iq_ab) from the sidekick's event feed, per band.

  python3 tools/decode_bench/score_device_ab.py EVENTS.jsonl SCHEDULE.log [YYYY-MM-DD]

EVENTS.jsonl is every /api/events event, one JSON object per line (deduplicated by its `s`);
SCHEDULE.log has one "HH:MM:SS start <band>:" line (UTC) per band change. How the 2026-11-03
run was recorded and scored: tools/decode_bench/corpus/qmx-2026-11-03-device/README.md.
A slot counts once the clock is set (slot index > 1e8) and only on the band the schedule had
it on. Turns are runs of slots in one mode; each IQ turn is paired with the audio turn after it
and each audio turn with the IQ turn after it, so a steady band trend cancels.
"""
import datetime, json, re, statistics as st, sys

ev = [json.loads(l) for l in open(sys.argv[1]) if l.strip() and "_err" not in l]
txt = [(e["t"], e["x"]) for e in ev if e.get("d") == 0]
sched = []
for line in open(sys.argv[2]):
    m = re.match(r"(\d\d:\d\d:\d\d) start (\S+):", line)
    if m:
        sched.append((m[1], m[2]))
day = datetime.date.fromisoformat(sys.argv[3]) if len(sys.argv) > 3 else datetime.date.today()
def epoch(hms):
    h, mi, s = map(int, hms.split(":"))
    return datetime.datetime(day.year, day.month, day.day, h, mi, s, tzinfo=datetime.timezone.utc).timestamp()
bounds = [(epoch(t), b) for t, b in sched]

slots, bdry, flags = {}, {}, {"kept": 0, "not sent": 0, "differs": 0, "q9": 0}
for t, x in txt:
    m = re.search(r"SLOT (\d+) c\d+/\d+ m(\d+) t\d+ dec(\d+) left(-?\d+).* rx=(iq|au)", x)
    if m and int(m[1]) > 1e8:
        slots[int(m[1])] = dict(m=int(m[2]), left=int(m[4]), rx=m[5])
    m = re.search(r"BDRY (\d+) g\d+ lost(\d+) drop(\d+)", x)
    if m and int(m[1]) > 1e8:
        bdry[int(m[1])] = (int(m[2]), int(m[3]))
    for k in ("kept", "not sent", "differs"):
        if k in x:
            flags[k] += 1
    if x.startswith("Q9") and "left" in x:
        flags["q9"] += 1

def band_of(slot):
    t = slot * 15
    cur = None
    for start, b in bounds:
        if start <= t < start + 30:  # the slot the band change lands in belongs to neither band
            return None
        if t >= start + 30:
            cur = b
    return cur

by_band = {}
for s in sorted(slots):
    b = band_of(s)
    if b:
        by_band.setdefault(b, []).append((s, slots[s]))
now = datetime.datetime.now(datetime.timezone.utc).strftime("%H:%M")
print(f"at {now} UTC; Q9 sends {flags['q9']}, read-back mismatches {flags['differs']}, not sent {flags['not sent']}, undone {flags['kept']}")
for b, rows in by_band.items():
    iq = [r["m"] for _, r in rows if r["rx"] == "iq"]
    au = [r["m"] for _, r in rows if r["rx"] == "au"]
    turns = []
    for s, r in rows:
        if turns and turns[-1][0] == r["rx"] and s == turns[-1][2] + 1:
            turns[-1][1].append(r["m"]); turns[-1][2] = s
        else:
            turns.append([r["rx"], [r["m"]], s])
    diffs = []
    for a, c in zip(turns, turns[1:]):
        if a[0] != c[0] and c[2] - len(c[1]) == a[2]:  # adjacent turns
            iqt, aut = (a, c) if a[0] == "iq" else (c, a)
            diffs.append(st.mean(iqt[1]) - st.mean(aut[1]))
    lost = [bdry[s][0] for s, _ in rows if s in bdry]
    drop = sum(bdry[s][1] for s, _ in rows if s in bdry)
    line = f"  {b}: IQ {st.mean(iq):.2f}/slot ({len(iq)})  audio {st.mean(au):.2f}/slot ({len(au)})" if iq and au else f"  {b}: too few slots"
    if len(diffs) > 2:
        se = st.stdev(diffs) / len(diffs) ** 0.5
        line += f"  paired IQ−audio {st.mean(diffs):+.2f} ± {se:.2f} per slot ({(100 * st.mean(diffs) / st.mean(au)) if st.mean(au) else float("nan"):+.1f}%, {len(diffs)} pairs)"
    line += f"  worst left {min(r['left'] for _, r in rows)}  drop {drop}  lost max {max(lost) if lost else '-'}"
    over = {k: sum(1 for _, r in rows if r["rx"] == k and r["left"] < 0) for k in ("iq", "au")}
    line += f"  past boundary iq {over['iq']}/{len(iq)} au {over['au']}/{len(au)}"
    print(line)
