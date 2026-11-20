#!/usr/bin/env python3
"""Measure PSKReporter's live feed for one 4-character grid (docs/architecture/system.md, Third-party services).

usage: pskr_grid_measure.py GRID MINUTES [--skip CALL] [--save OUT.jsonl]

Subscribes to FT8 spots sent from GRID and heard in GRID over plain MQTT (mqtt.pskreporter.info:1883, the
feed the app reads over WebSocket), then prints what a band-conditions card would cost and show: spots and
bytes per minute, how long a spot takes to reach the feed, how fast the window fills, and per band the distinct
stations by distance. --skip leaves a call out of the counts (your own: it swamps your grid while you
transmit). --save keeps every spot as JSON lines, to re-read or to use as a test fixture.
"""
import argparse, json, math, socket, struct, sys, time
from collections import Counter

BROKER = ('mqtt.pskreporter.info', 1883)
NEAR_KM, FAR_KM = 500, 3000


def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (128 if n else 0))
        if not n:
            return bytes(out)


def utf8(s):
    b = s.encode()
    return struct.pack('>H', len(b)) + b


def packet(first, body):
    return bytes([first]) + enc_len(len(body)) + body


def recv_exact(sock, n):
    out = b''
    while len(out) < n:
        c = sock.recv(n - len(out))
        if not c:
            raise EOFError('broker closed the connection')
        out += c
    return out


def read_packet(sock):
    first = recv_exact(sock, 1)[0]
    mult, n = 1, 0
    while True:
        b = recv_exact(sock, 1)[0]
        n += (b & 127) * mult
        mult *= 128
        if not b & 128:
            break
    return first, recv_exact(sock, n)


def latlon(grid):
    g = grid.upper()
    return (ord(g[1]) - 65) * 10 - 90 + int(g[3]) + 0.5, (ord(g[0]) - 65) * 20 - 180 + int(g[2]) * 2 + 1


def km(a, b):
    p1, l1, p2, l2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin((l2 - l1) / 2) ** 2
    return 6371 * 2 * math.asin(math.sqrt(h))


def record(grid, minutes, save):
    # Topic levels after v2: band, mode, sender call, receiver call, sender locator, receiver locator (4 characters
    # each), sender and receiver entity.
    topics = ['pskr/filter/v2/+/FT8/+/+/%s/#' % grid, 'pskr/filter/v2/+/FT8/+/+/+/%s/#' % grid]
    sock = socket.create_connection(BROKER, timeout=15)  # its CONNACK took over 5 s twice on 2026-11-20
    client = 'mft8-measure-%d' % int(time.time())
    sock.sendall(packet(0x10, utf8('MQTT') + bytes([4, 2]) + struct.pack('>H', 60) + utf8(client)))
    if read_packet(sock)[0] >> 4 != 2:
        sys.exit('the broker did not accept the connection')
    sock.settimeout(5)
    for i, t in enumerate(topics, 1):
        sock.sendall(packet(0x82, struct.pack('>H', i) + utf8(t) + b'\0'))
    start = time.time()
    end, last_ping, spots = start + minutes * 60, start, []
    out = open(save, 'w') if save else None
    while time.time() < end:
        if time.time() - last_ping > 30:
            sock.sendall(b'\xc0\x00')
            last_ping = time.time()
        try:
            first, body = read_packet(sock)
        except socket.timeout:
            continue
        if first >> 4 != 3:
            continue
        tl = struct.unpack('>H', body[:2])[0]
        off = 2 + tl + (2 if (first >> 1) & 3 else 0)
        spot = json.loads(body[off:].decode())
        spot['arrived'] = round(time.time(), 1)
        spot['bytes'] = len(body) + 2
        spots.append(spot)
        if out:
            out.write(json.dumps(spot) + '\n')
    if out:
        out.close()
    sock.sendall(b'\xe0\x00')
    sock.close()
    return start, time.time(), spots


def band_metres(band):
    try:
        return float(band[:-2]) / 100 if band.endswith('cm') else float(band[:-1])
    except ValueError:
        return 0.0


def report(grid, start, stop, spots, skip):
    secs = max(stop - start, 1)
    home = latlon(grid)
    print('grid %s, %.1f min from %s UTC, %d spots' % (grid, secs / 60, time.strftime('%Y-%m-%d %H:%M', time.gmtime(start)), len(spots)))
    print('both directions: %.2f spots/s, %.0f bytes/min' % (len(spots) / secs, sum(s['bytes'] for s in spots) / secs * 60))
    lag = sorted(s['arrived'] - s['t_tx'] for s in spots if s.get('t_tx'))
    if lag:
        print('reaching the feed after the transmission: median %.0f s, 90th percentile %.0f s, longest %.0f s'
              % (lag[len(lag) // 2], lag[int(len(lag) * 0.9)], lag[-1]))
    ways = (('sent from', lambda s: s['sl'][:4].upper() == grid, 'rc', 'rl'),
            ('heard in', lambda s: s['rl'][:4].upper() == grid, 'sc', 'sl'))
    for name, mine, other, other_grid in ways:
        rows = [s for s in spots if mine(s) and s['sc'] != skip and s['rc'] != skip]
        print('\n%s %s: %d spots%s' % (name, grid, len(rows), ', %s left out' % skip if skip else ''))
        if not rows:
            continue
        side = 'sc' if name == 'sent from' else 'rc'
        print('  busiest %s: %s' % ('senders' if side == 'sc' else 'receivers',
                                     ', '.join('%s %d' % kv for kv in Counter(s[side] for s in rows).most_common(5))))
        seen = {}
        for s in rows:
            seen.setdefault((s['b'], s[other]), s['arrived'] - start)
        print('  share of the run\'s stations seen by minute: ' + ', '.join(
            '%d: %d%%' % (m, 100 * sum(v <= m * 60 for v in seen.values()) / len(seen)) for m in (2, 5, 10, 15)))
        print('  distinct stations by distance (under %d km / to %d km / beyond):' % (NEAR_KM, FAR_KM))
        for band in sorted({s['b'] for s in rows}, key=band_metres, reverse=True):
            dist = {}
            for s in rows:
                if s['b'] == band and len(s.get(other_grid, '')) >= 4:
                    dist[s[other]] = km(home, latlon(s[other_grid]))
            c = Counter('near' if d < NEAR_KM else 'mid' if d < FAR_KM else 'far' for d in dist.values())
            print('    %-5s %4d %4d %4d' % (band, c['near'], c['mid'], c['far']))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('grid')
    ap.add_argument('minutes', type=float)
    ap.add_argument('--skip', default='', help='a call to leave out of the counts')
    ap.add_argument('--save', help='write every spot to this JSON-lines file')
    a = ap.parse_args()
    grid = a.grid.upper()[:4]
    if len(grid) != 4:
        sys.exit('GRID is a 4-character locator, such as EM10')
    report(grid, *record(grid, a.minutes, a.save), a.skip.upper())


if __name__ == '__main__':
    main()
