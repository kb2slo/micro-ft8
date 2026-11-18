#!/usr/bin/env python3
"""A client slow to take its reply, as a phone in WiFi power save is (roadmap row uwzn): for SECONDS it GETs
PATH again and again with a small receive buffer, reading CHUNK bytes every GAP seconds, so the sidekick's
send waits on it. One line per request to stdout: UTC time, bytes, seconds it took. README.md says how it
is used beside probe.py.

    python3 tools/sidekick_load/slow_client.py http://192.168.50.128 60 /app.html 1024 0.25"""
import socket, sys, time, urllib.parse

host, seconds, path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
chunk = int(sys.argv[4]) if len(sys.argv) > 4 else 1024
gap = float(sys.argv[5]) if len(sys.argv) > 5 else 0.25
u = urllib.parse.urlsplit(host); addr, port = u.hostname, u.port or 80
end = time.time() + seconds
while time.time() < end:
    t0 = time.time(); got = 0
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Before connect, so the window advertised in the handshake is already small.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, chunk)
    s.settimeout(30)
    try:
        s.connect((addr, port))
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: {addr}\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n".encode())
        while time.time() < end:
            b = s.recv(chunk)
            if not b:
                break
            got += len(b)
            time.sleep(gap)
        print(f"{time.strftime('%H:%M:%S', time.gmtime(t0))} {got} B {time.time() - t0:.1f} s", flush=True)
    except OSError as e:
        print(f"{time.strftime('%H:%M:%S', time.gmtime(t0))} FAIL {type(e).__name__} after {got} B", flush=True)
    finally:
        s.close()
