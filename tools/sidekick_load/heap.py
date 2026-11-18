#!/usr/bin/env python3
"""Every 5 s for MINUTES: the sidekick's heap and server facts from /api/config, one JSON line each to OUT.

    python3 tools/sidekick_load/heap.py http://192.168.50.128 10.5 heap.jsonl"""
import json, sys, time, urllib.request
host, minutes, out_path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
keys = ("sk_fw_version", "sk_uptime", "sk_heap_free", "sk_heap_min", "sk_heap_largest", "sk_httpd_held_peak_ms",
        "sk_worker_queue_peak", "sk_tcp_used", "sk_tcp_held", "sk_tcp_held_peak", "sk_tcp_backlog_peak", "sk_ring_gaps",
        "sk_crc_errors", "sk_send_queue_peak", "sk_files_wait_peak", "sk_send_stack_free", "sk_send_cuts")
out = open(out_path, "a", buffering=1); end = time.time() + 60 * minutes
while time.time() < end:
    stamp = time.strftime("%H:%M:%S", time.gmtime())
    try:
        d = json.load(urllib.request.urlopen(host.rstrip("/") + "/api/config", timeout=8))
        out.write(json.dumps({"t": stamp, **{k: d.get(k) for k in keys}}) + "\n")
    except Exception as e:
        out.write(json.dumps({"t": stamp, "err": type(e).__name__}) + "\n")
    time.sleep(5)
