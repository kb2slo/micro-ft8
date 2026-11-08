#!/usr/bin/env python3
"""Sidekick web latency (row dlu3; docs/architecture/sidekick.md, Port A). Times N sequential GETs of /api/events on the sidekick, and of /api/config: median, 90th, max, failures.

    python3 tools/sidekick_latency.py [N] [http://micro-ft8.local]

follow: since= the last reply's seq, as the app polls. catchup: since=1 every time, a full ring, the worst case."""
import json, sys, time, urllib.request
N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
HOST = sys.argv[2] if len(sys.argv) > 2 else "http://micro-ft8.local"
def get(path):
    t0 = time.time()
    try:
        body = urllib.request.urlopen(HOST + path, timeout=10).read(); return (time.time() - t0) * 1000, body
    except Exception:
        return (time.time() - t0) * 1000, None
def report(name, ms, bad, nbytes):
    ms.sort(); n = len(ms)
    print(f"{name:15} n={n} median {ms[n//2]:.0f} ms, 90th {ms[int(n*0.9)]:.0f} ms, max {ms[-1]:.0f} ms, failed {bad}, mean reply {nbytes//max(1,n-bad)} B")
seq = json.load(urllib.request.urlopen(HOST + "/api/events?since=0", timeout=10))["seq"]
for mode in ("follow", "catchup"):
    ms, bad, nbytes = [], 0, 0
    for _ in range(N):
        t, body = get("/api/events?since=%d" % (seq if mode == "follow" else 1))
        ms.append(t)
        if body is None: bad += 1
        else:
            nbytes += len(body)
            if mode == "follow": seq = json.loads(body)["seq"]
        time.sleep(0.2)
    report("events " + mode, ms, bad, nbytes)
ms, bad, nbytes = [], 0, 0
for _ in range(N):
    t, body = get("/api/config"); ms.append(t); bad += body is None; nbytes += len(body or b"")
    time.sleep(0.2)
report("config", ms, bad, nbytes)
cfg = json.load(urllib.request.urlopen(HOST + "/api/config", timeout=10))
print("sk", cfg.get("sk_fw_version"), "uptime", cfg.get("sk_uptime"), "heap", cfg.get("sk_heap_free"), "min", cfg.get("sk_heap_min"))
