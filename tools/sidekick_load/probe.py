#!/usr/bin/env python3
"""Reads /api/events on a new connection every GAP seconds for SECONDS, one line per read to OUT:
UTC time, connect ms, reply ms, events (or FAIL, ms to failure, error). With LOG_OUT, also every log line in
the ring, the sidekick's (`SK ...`: heap lows, slow requests, held server, cut sends) and the main board's (slot
timing, audio lost, stalls), and each radio state, with the read's time. README.md says how it is used.

    python3 tools/sidekick_load/probe.py http://192.168.50.128 600 0.5 probe.log [log.txt]"""
import http.client, json, sys, time, urllib.parse
host, seconds, gap, out_path = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
u = urllib.parse.urlsplit(host); addr, port = u.hostname, u.port or 80
out = open(out_path, "a", buffering=1); seq, end = None, time.time() + seconds
sk = open(sys.argv[5], "a", buffering=1) if len(sys.argv) > 5 else None
while time.time() < end:
    t0 = time.time(); stamp = time.strftime("%H:%M:%S", time.gmtime(t0)) + ".%01d" % int(t0 % 1 * 10)
    c = http.client.HTTPConnection(addr, port, timeout=10)
    try:
        c.connect(); t1 = time.time()
        c.request("GET", "/api/events?since=%d" % (seq or 0)); d = json.loads(c.getresponse().read()); t2 = time.time()
        if sk and seq is not None:
            if d["events"] and d["events"][0].get("s", seq) > seq:
                sk.write(f"{stamp} GAP {d['events'][0]['s'] - seq} events not seen\n")
            for e in d["events"]:
                if e.get("d") == 0:
                    sk.write(f"{stamp} {e.get('x', '')}\n")
                elif e.get("d") == 7:
                    sk.write(f"{stamp} RADIO {json.dumps(e)}\n")
        seq = d["seq"]; out.write(f"{stamp} {1000*(t1-t0):.0f} {1000*(t2-t1):.0f} {len(d['events'])}\n")
    except Exception as e:
        out.write(f"{stamp} FAIL {1000*(time.time()-t0):.0f} {type(e).__name__}\n")
    finally:
        c.close()
    time.sleep(gap)
