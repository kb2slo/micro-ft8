#!/usr/bin/env python3
"""Sidekick web latency (docs/architecture/sidekick.md, Port A, Tasks and cores). Times N sequential GETs of /api/events on the sidekick, and of /api/config: median, 90th, max, failures. Each GET is a new connection, timed in two parts: the TCP connect, and the reply once connected.

    python3 tools/sidekick_latency.py [N] [http://micro-ft8.local]

follow: since= the last reply's seq, as the app polls. catchup: since=1 every time, a full ring, the worst case.
A connect near 1, 3 or 7 s is a SYN the client had to send again: the network lost it, or the sidekick dropped it
(sk_tcp_held_peak and sk_tcp_backlog_peak in /api/config say whether it was out of connections)."""
import http.client, json, sys, time, urllib.parse
N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
HOST = sys.argv[2] if len(sys.argv) > 2 else "http://micro-ft8.local"
_u = urllib.parse.urlsplit(HOST)
ADDR, PORT = _u.hostname, _u.port or 80
def get(path):
    """(connect ms, reply ms, body); a failed step's ms is its time to failure, and later ones None."""
    t0 = time.time(); c = http.client.HTTPConnection(ADDR, PORT, timeout=10)
    try:
        c.connect()
    except Exception:
        c.close(); return (time.time() - t0) * 1000, None, None
    t1 = time.time()
    try:
        c.request("GET", path); body = c.getresponse().read()
        return (t1 - t0) * 1000, (time.time() - t1) * 1000, body
    except Exception:
        return (t1 - t0) * 1000, (time.time() - t1) * 1000, None
    finally:
        c.close()
def stats(ms):
    ms = sorted(ms); n = len(ms)
    return f"median {ms[n//2]:.0f} ms, 90th {ms[int(n*0.9)]:.0f} ms, max {ms[-1]:.0f} ms" if n else "none"
def report(name, rows):
    conn = [r[0] for r in rows if r[1] is not None]
    reply = [r[1] for r in rows if r[2] is not None]
    no_conn = sum(r[1] is None for r in rows); no_reply = sum(r[1] is not None and r[2] is None for r in rows)
    nbytes = sum(len(r[2]) for r in rows if r[2] is not None)
    print(f"{name:15} n={len(rows)} connect {stats(conn)}; reply {stats(reply)}; failed {no_conn} at connect, "
          f"{no_reply} in reply; mean reply {nbytes // max(1, len(reply))} B")
seq = json.loads(get("/api/events?since=0")[2])["seq"]
for mode in ("follow", "catchup"):
    rows = []
    for _ in range(N):
        r = get("/api/events?since=%d" % (seq if mode == "follow" else 1))
        rows.append(r)
        if mode == "follow" and r[2] is not None:
            seq = json.loads(r[2])["seq"]
        time.sleep(0.2)
    report("events " + mode, rows)
rows = []
for _ in range(N):
    rows.append(get("/api/config")); time.sleep(0.2)
report("config", rows)
cfg = json.loads(get("/api/config")[2])
print("sk", cfg.get("sk_fw_version"), "uptime", cfg.get("sk_uptime"), "heap", cfg.get("sk_heap_free"), "min", cfg.get("sk_heap_min"),
      "tcp", cfg.get("sk_tcp_used"), "held", cfg.get("sk_tcp_held"), "held peak", cfg.get("sk_tcp_held_peak"), "backlog peak", cfg.get("sk_tcp_backlog_peak"),
      "worker queue", cfg.get("sk_worker_queue_peak"),
      "httpd held peak ms", cfg.get("sk_httpd_held_peak_ms"))
