#!/usr/bin/env python3
"""Record an HL2's receivers as I/Q WAVs, one per 15 s FT8 slot per receiver, for scoring against `jt9`.

The HL2 counterpart of tools/qmx_record.py, and built on it: that file's Recorder and SlotWriter cut
the stream on UTC slot boundaries into the same 48 kHz, 2-channel, 24-bit WAVs and the same
manifest.jsonl, one Recorder per receiver, so tools/decode_bench/score_ab.py scores a band unchanged:

  python3 tools/decode_bench/score_ab.py DIR/40m --center-hz 1500 --iq-flag=--negate-q

What this file adds is the HL2's side: its Protocol 1 wire format and a UDP stream that looks to each
Recorder like an audio stream. The HL2 is driven as the P4 drives it (poe_p4/main/hl2_transport.cpp):
48 kHz, each receiver's NCO on its dial, the N2ADR's filters, the LNA from +20 dB stepped down 3 dB a
second while the ADC clips, and one control packet sent back for each packet received. I is written as
sent on the left channel and Q as sent on the right; the HL2's Q is the negative of the usual
convention, hence the scorer's --negate-q, as the P4 does (docs/architecture/decode.md, **The HL2's
channel**). A sequence gap is zero-filled with that many packets' samples, so the timeline holds, and
counts as an overflow in the manifest.

**Several receivers share one set of filters and one LNA.** The N2ADR byte is the highest band's: its
low-pass passes every lower band, so they lose their own, and the receive high-pass stays in line unless
every band is 160 m. One band alone gets its own filter, as on the P4 today. Recording the same bands
both ways measures what the sharing costs (docs/roadmap/hl2-support.md; RFC 0016, S6).

Each manifest line carries the radio-wide figures for its slot, the same for every receiver: packets
lost and late, ADC status words and how many read clipped, the LNA's range, the HL2's temperature
range, and which receiver and filter byte recorded it.

Receive only: every packet it sends has MOX clear and zero transmit samples, and it writes only the
receive registers (config, the receivers' NCOs, the LNA). The wire format is the codec's,
components/hl2_proto; tools/test_hl2_record.py holds this file to the same golden frames.

The HL2 must be on a network this computer reaches, with no other client streaming from it (Protocol 1
streams to one host). On a direct cable or a switch with no DHCP server it takes
169.254.MAC[15:8].MAC[7:0], so give this computer a link-local address on that interface first and
pass it as --bind. The clock must be synced to well under a second: files are cut on UTC boundaries.

  python3 tools/hl2_record.py --bind 169.254.10.1 --dial-hz 7074000 --minutes 30 --out DIR

Or a plan, run in order: each entry is DIAL:SLOTS, or up to four dials joined by + to record them at
once, and optionally @HH:MM, a UTC time it waits for if it comes up earlier (night bands after dark).
Each band goes in its own directory under --out, named for the band:

  python3 tools/hl2_record.py --bind 169.254.10.1 --out DIR \\
      --plan 28074000:120,14074000:120,7074000:120@00:00
  python3 tools/hl2_record.py --bind 169.254.10.1 --out DIR \\
      --plan 28074000+21074000+14074000+7074000:120

For a run nobody watches: a lost stream (no packet for a second) drops the unfinished slots, waits out
the HL2's watchdog, finds the radio again and carries on with that entry's remaining slots; the HL2
over --max-temp-c, a disk with too little room for the next entry, or a clock that says it is not
synced stops the run.

Python 3 only, no packages. Ctrl-C once: finish the current slot, then stop. Twice: stop now and
delete the unfinished .wav.part files.

Host test, no radio needed:

  python3 tools/test_hl2_record.py
"""
import argparse
import datetime
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmx_record  # noqa: E402  the slot cutting, manifest and Ctrl-C handling

PORT = 1024
PACKET_LEN = 1032
FRAME_LEN = 512
MAX_RECEIVERS = 4  # the standard hl2b5up_main gateware
BOARD_ID = 0x06
REG_CONFIG, REG_NCO0, REG_LNA = 0x00, 0x02, 0x0A
LNA_START_DB, LNA_FLOOR_DB, LNA_STEP_DB = 20, -12, 3
MAX_FILL_PACKETS = 400  # past ~1 s of loss the stream is lost anyway, as on the P4
STREAM_LOST_S = 1.0
WATCHDOG_WAIT_S = 50  # the HL2 reports running for ~43 s after a host goes quiet (bench, 2026-11-03)
SLOT_BYTES = qmx_record.FRAMES_PER_SLOT * qmx_record.FRAME_BYTES + 44
BANDS = ((1800000, 2000000, "160m"), (3500000, 4000000, "80m"), (5330000, 5410000, "60m"),
         (7000000, 7300000, "40m"), (10100000, 10150000, "30m"), (14000000, 14350000, "20m"),
         (18068000, 18168000, "17m"), (21000000, 21450000, "15m"), (24890000, 24990000, "12m"),
         (28000000, 29700000, "10m"))


# ---- The wire format (components/hl2_proto/hl2_proto.cpp) -------------------------------------------------------

def samples_per_frame(receivers):
    """floor(504 / (6 N + 2)): 63 for one receiver, 19 for four."""
    return 504 // (6 * receivers + 2)


def discovery_request():
    return bytes([0xEF, 0xFE, 0x02]) + bytes(60)


def parse_discovery_reply(d):
    """The reply's fields, or None unless d is a 60-byte HL2 reply."""
    if len(d) != 60 or d[0] != 0xEF or d[1] != 0xFE or d[10] != BOARD_ID or d[2] not in (0x02, 0x03):
        return None
    return {"running": d[2] == 0x03, "mac": d[3:9].hex(":"), "gateware": f"{d[9]}.{d[21]}",
            "receivers": d[19], "temperature_c": temperature_c(struct.unpack(">H", d[28:30])[0])}


def temperature_c(code):
    return round((3.26 * (code & 0x0FFF) / 4096 - 0.5) / 0.01, 1)


def start_packet():
    return bytes([0xEF, 0xFE, 0x04, 0x01]) + bytes(60)


def stop_packet():
    return bytes([0xEF, 0xFE, 0x04, 0x00]) + bytes(60)


def n2adr_receive_filters(hz):
    """The N2ADR byte for receiving at hz: its low-pass, and b6 (the receive high-pass bypassed) on 160 m only.
    For several receivers pass the highest dial: its low-pass passes every lower band."""
    bit = 5
    for limit, b in ((2500000, 0), (4500000, 1), (8000000, 2), (15000000, 3), (22000000, 4)):
        if hz < limit:
            bit = b
            break
    return (1 << bit) | ((1 << 6) if hz < 2500000 else 0)


def config_register(n2adr, receivers=1):
    """Register 0x00: 48 kHz, the last receiver in [6:3], duplex, the N2ADR byte in [23:17]."""
    if not 1 <= receivers <= MAX_RECEIVERS:
        raise ValueError(f"{receivers} receivers: the gateware has 1 to {MAX_RECEIVERS}")
    return REG_CONFIG, ((n2adr & 0x7F) << 17) | ((receivers - 1) << 3) | (1 << 2)


def nco_register(hz, receiver=0):
    if not 0 <= hz <= 38400000:
        raise ValueError(f"{hz} Hz is outside the HL2's range")
    if not 0 <= receiver < MAX_RECEIVERS:
        raise ValueError(f"no receiver {receiver + 1}")
    return REG_NCO0 + receiver, hz


def lna_register(gain_db):
    if not -12 <= gain_db <= 48:
        raise ValueError(f"LNA {gain_db} dB is outside -12..+48")
    return REG_LNA, (1 << 6) | (gain_db + 12)


def control_packet(sequence, first, second):
    """A 1032-byte EP2 packet: two register writes, MOX clear, every transmit sample zero."""
    out = bytearray(PACKET_LEN)
    out[0:8] = bytes([0xEF, 0xFE, 0x01, 0x02]) + struct.pack(">I", sequence & 0xFFFFFFFF)
    for i, (address, value) in enumerate((first, second)):
        if address not in (REG_CONFIG, REG_LNA) and not REG_NCO0 <= address < REG_NCO0 + MAX_RECEIVERS:
            raise ValueError(f"register 0x{address:02x} is not a receive register")
        if address == REG_CONFIG and value & (1 << 16):
            raise ValueError("register 0x00's PA-enable bit is never set")
        f = 8 + i * FRAME_LEN
        out[f:f + 8] = bytes([0x7F, 0x7F, 0x7F, (address & 0x3F) << 1]) + struct.pack(">I", value)
    return bytes(out)


def parse_rx_packet(d, receivers=1):
    """(sequence, [WAV frames per receiver: little-endian 24-bit I then Q], [(status address, C1..C4)]),
    or None. Each sample is every receiver's I and Q in turn, then a 2-byte mic word."""
    if len(d) != PACKET_LEN or d[0:4] != b"\xef\xfe\x01\x06":
        return None
    per_frame, stride = samples_per_frame(receivers), 6 * receivers + 2
    sequence = struct.unpack(">I", d[4:8])[0]
    pcm = [bytearray(2 * per_frame * 6) for _ in range(receivers)]
    status = []
    w = 0
    for f in (8, 8 + FRAME_LEN):
        if d[f:f + 3] != b"\x7f\x7f\x7f":
            return None
        c0 = d[f + 3]
        if not c0 & 0x80:  # a status word, not a command response
            status.append(((c0 >> 3) & 0x03, d[f + 4:f + 8]))
        s = f + 8
        for _ in range(per_frame):
            for r in range(receivers):
                o = s + 6 * r
                pcm[r][w:w + 3] = d[o:o + 3][::-1]
                pcm[r][w + 3:w + 6] = d[o + 3:o + 6][::-1]
            w += 6
            s += stride
    return sequence, [bytes(p) for p in pcm], status


def sample_values(pcm):
    """Signed ints from little-endian 24-bit samples, for tests."""
    return [int.from_bytes(pcm[i:i + 3], "little", signed=True) for i in range(0, len(pcm), 3)]


# ---- The stream --------------------------------------------------------------------------------------------------

class _Status:
    """What qmx_record.Recorder reads of a block's status: an overflow, here a sequence gap."""

    def __init__(self, gap):
        self.input_overflow = gap

    def __bool__(self):
        return self.input_overflow


class _Tap:
    """Receiver k's stream as its own Recorder sees it: only receiver 0's Recorder starts and stops the radio."""

    def __init__(self, stream, k):
        self.stream, self.k = stream, k

    def attach(self, callback):
        self.stream.callbacks[self.k] = callback
        return self.stream if self.k == 0 else self

    def start(self):
        pass

    def stop(self):
        pass

    def close(self):
        pass


class Hl2Stream:
    """The HL2 as qmx_record.Recorder's open_stream() object: start(), stop(), close()."""

    def __init__(self, sock, hl2, dials, log=print, clock=time.time, max_temp_c=55.0, start_utc=None):
        self.sock, self.hl2, self.log, self.clock = sock, hl2, log, clock
        self.dials = list(dials)
        self.receivers = len(self.dials)
        self.max_temp_c = max_temp_c
        self.start_utc = start_utc  # slot 0's boundary: the per-slot counts are indexed from it
        self.lost = False  # set when the stream stopped arriving
        self.too_hot = None  # the temperature that stopped the run
        self.n2adr = n2adr_receive_filters(max(self.dials))
        self.config = config_register(self.n2adr, self.receivers)
        self.ncos = [nco_register(hz, k) for k, hz in enumerate(self.dials)]
        self.lna_db = LNA_START_DB
        self.lna = lna_register(self.lna_db)
        self.fill = bytes(2 * samples_per_frame(self.receivers) * qmx_record.FRAME_BYTES)
        self.callbacks = [None] * self.receivers
        self.recorders = []  # set by the caller; an abort goes to every one
        self.tx_seq = 0
        self.expected = None
        self.window = [0, 0]  # address-0 status words, of them clipped, this second
        self.next_lna = None
        self.stats = {}  # slot index -> radio-wide counts for that slot
        self._stop = threading.Event()
        self._thread = None

    def tap(self, k):
        return _Tap(self, k).attach

    def _slot(self, now):
        if self.start_utc is None or now < self.start_utc:
            return None
        i = int((now - self.start_utc) // qmx_record.SLOT_S)
        if i not in self.stats:
            self.stats[i] = {"radio": "HL2", "receivers": self.receivers, "dials_hz": self.dials,
                             "n2adr": self.n2adr, "lna_db_start": self.lna_db, "lna_db_end": self.lna_db,
                             "status_words": 0, "clipped_status_words": 0, "lost_packets": 0,
                             "late_packets": 0, "temp_c_min": None, "temp_c_max": None}
        return self.stats[i]

    def slot_stats(self, k, rec):
        """Receiver k's SlotWriter extra hook: the radio-wide counts of the slot it is closing."""
        st = dict(self.stats.get(rec.writer.index) or {})
        st["receiver"] = k + 1
        return st

    def _abort(self):
        """End the recording now: each Recorder's writer drops its unfinished slot and run() returns."""
        for r in self.recorders:
            r.q.put(("abort",))

    def _send_control(self, first):
        second = self.ncos[self.tx_seq % self.receivers]
        self.sock.sendto(control_packet(self.tx_seq, first, second), self.hl2)
        self.tx_seq += 1

    def handle(self, d, now):
        """One received datagram: returns ([frames per receiver], gap) to hand on, or None to skip it."""
        p = parse_rx_packet(d, self.receivers)
        if p is None:
            return None
        sequence, pcm, status = p
        slot = self._slot(now)
        missing = 0 if self.expected is None else (sequence - self.expected) & 0xFFFFFFFF
        if missing >= 1 << 31:
            # A duplicate or a late packet: its place was already written, zero-filled if it was missing,
            # so its samples are dropped and the next one expected stays where it was.
            if slot is not None:
                slot["late_packets"] += 1
            return None
        if missing > MAX_FILL_PACKETS:  # a restart, or more loss than the stream survives: no fill
            missing = 0
        self.expected = (sequence + 1) & 0xFFFFFFFF
        for address, c in status:
            if address == 1:
                t = temperature_c(struct.unpack(">H", c[0:2])[0])
                if slot is not None:
                    lo, hi = slot["temp_c_min"], slot["temp_c_max"]
                    slot["temp_c_min"] = t if lo is None else min(lo, t)
                    slot["temp_c_max"] = t if hi is None else max(hi, t)
                if t > self.max_temp_c and self.too_hot is None:
                    self.too_hot = t
                    self.log(f"HL2 at {t} C, over --max-temp-c {self.max_temp_c}: stopping")
                    self._abort()
            elif address == 0:
                clipped = bool(c[0] & 0x01)
                self.window[0] += 1
                self.window[1] += clipped
                if slot is not None:
                    slot["status_words"] += 1
                    slot["clipped_status_words"] += clipped
        if self.next_lna is None:
            self.next_lna = now + 1.0
        elif now >= self.next_lna:
            if self.window[1] * 100 > self.window[0] and self.lna_db > LNA_FLOOR_DB:
                self.lna_db = max(LNA_FLOOR_DB, self.lna_db - LNA_STEP_DB)
                self.lna = lna_register(self.lna_db)
                self.log(f"ADC clipped in {self.window[1]} of {self.window[0]} status words: "
                         f"LNA {self.lna_db:+d} dB")
            self.window = [0, 0]
            self.next_lna = now + 1.0
        if slot is not None:
            slot["lna_db_end"] = self.lna_db
            slot["lost_packets"] += missing
        # Each packet carries, in turn, the config (filters) or the LNA, and one receiver's NCO.
        self._send_control(self.lna if self.tx_seq & 1 else self.config)
        return [self.fill * missing + p for p in pcm], missing > 0

    def start(self):
        for i in range(max(4, 2 * self.receivers)):
            self._send_control(self.lna if i & 1 else self.config)
        self.sock.sendto(start_packet(), self.hl2)
        self.log(f"streaming {self.receivers} receiver(s) at 48 kHz, NCOs {', '.join(str(h) for h in self.dials)} Hz, "
                 f"N2ADR 0x{self.n2adr:02x}, LNA {self.lna_db:+d} dB")
        self._thread = threading.Thread(target=self._run, name="hl2-record-rx", daemon=True)
        self._thread.start()

    def _run(self):
        self.sock.settimeout(0.2)
        last = self.clock()
        while not self._stop.is_set():
            try:
                d, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                if self.clock() - last > STREAM_LOST_S:
                    self.log("stream lost: no packet for a second")
                    self.lost = True
                    self._abort()
                    return
                continue
            now = self.clock()
            out = self.handle(d, now)
            if out is None:
                continue
            last = now
            frames, gap = out
            for cb, f in zip(self.callbacks, frames):
                if cb is not None:
                    cb(f, len(f) // qmx_record.FRAME_BYTES, None, _Status(gap))

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        try:
            self.sock.sendto(stop_packet(), self.hl2)
        except OSError:
            pass  # the link is down; the HL2's watchdog stops it

    def close(self):
        self.sock.close()


def discover(sock, host, timeout=3.0):
    """(address, reply fields) of the first HL2 to answer, or (None, None)."""
    end = time.time() + timeout
    sock.settimeout(0.5)
    while time.time() < end:
        sock.sendto(discovery_request(), (host or "255.255.255.255", PORT))
        try:
            while True:
                d, addr = sock.recvfrom(2048)
                r = parse_discovery_reply(d)
                if r:
                    return addr[0], r
        except socket.timeout:
            continue
    return None, None


# ---- The run -----------------------------------------------------------------------------------------------------

def band_name(hz):
    for lo, hi, name in BANDS:
        if lo <= hz <= hi:
            return name
    return f"{hz}Hz"


def parse_plan(text):
    """[([dial_hz, ...], slots, not_before (HH, MM) or None)] from DIAL[+DIAL...]:SLOTS[@HH:MM],..."""
    plan = []
    for item in text.split(","):
        m = re.fullmatch(r"\s*(\d+(?:\+\d+)*):(\d+)(?:@(\d{1,2}):(\d{2}))?\s*", item)
        if not m:
            raise ValueError(f"plan entry {item!r} is not DIAL[+DIAL...]:SLOTS[@HH:MM]")
        dials, slots = [int(x) for x in m.group(1).split("+")], int(m.group(2))
        if len(dials) > MAX_RECEIVERS:
            raise ValueError(f"plan entry {item!r}: at most {MAX_RECEIVERS} dials at once")
        if len({band_name(d) for d in dials}) != len(dials):
            raise ValueError(f"plan entry {item!r}: two dials in one band would share its directory")
        for d in dials:
            nco_register(d)
        if slots < 1:
            raise ValueError(f"plan entry {item!r} has no slots")
        when = (int(m.group(3)), int(m.group(4))) if m.group(3) else None
        if when and not (when[0] < 24 and when[1] < 60):
            raise ValueError(f"plan entry {item!r} has no such time")
        plan.append((dials, slots, when))
    return plan


def next_utc(hour, minute, now):
    """Epoch seconds of the HH:MM UTC nearest now, from 12 h before it to 12 h after: a time up to 12 h
    past (already dark) returns in the past, so nothing waits; a later one is waited for."""
    d = datetime.datetime.fromtimestamp(now, datetime.timezone.utc)
    t = d.replace(hour=hour, minute=minute, second=0, microsecond=0)
    for days in (-1, 0, 1):
        c = (t + datetime.timedelta(days=days)).timestamp()
        if now - 12 * 3600 <= c < now + 12 * 3600:
            return c
    return t.timestamp()


def clock_synced():
    """True, False, or None where this OS does not say (timedatectl on Linux)."""
    try:
        out = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"], capture_output=True,
                             text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return {"yes": True, "no": False}.get(out)


def open_hl2(bind, host, take):
    """(socket, HL2 address, discovery fields), or (None, None, reason)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    sock.bind((bind, 0))
    addr, info = discover(sock, host)
    if not addr:
        sock.close()
        return None, None, "no HL2 answered discovery: check the cable, --bind's interface, and that nothing else streams"
    if info["running"] and not take:
        sock.close()
        return None, None, "the HL2 reports it is streaming: wait for its watchdog (about 45 s) or pass --take"
    return sock, addr, info


def record_entry(args, dials, slots, out_dirs, log=print):
    """Records one plan entry's slots on every receiver at once, carrying on after a lost stream.
    Returns why it stopped early, or None."""
    for d in out_dirs:
        os.makedirs(d, exist_ok=True)
    done = 0
    take = args.take
    while done < slots:
        need = (slots - done) * SLOT_BYTES * len(dials) * 1.2 + (1 << 30)
        if shutil.disk_usage(out_dirs[0]).free < need:
            return f"not enough disk for {slots - done} more slots"
        sock, host, info = open_hl2(args.bind, args.host, take)
        if sock is None:
            return info
        log(f"HL2 at {host}: gateware {info['gateware']}, {info['temperature_c']} C; "
            f"{' + '.join(band_name(d) for d in dials)}, {slots - done} slot(s)")
        start = qmx_record.next_boundary(time.time() + qmx_record.LEAD_S)
        stream = Hl2Stream(sock, (host, PORT), dials, log=log, max_temp_c=args.max_temp_c, start_utc=start)
        recs = []
        for k, (dial, out_dir) in enumerate(zip(dials, out_dirs)):
            rec = qmx_record.Recorder(stream.tap(k), dict(
                out_dir=out_dir, slots=slots - done, mode_tag="iq", dial_hz=dial,
                version=qmx_record.recorder_version(), log=log if k == 0 else (lambda *_: None)),
                start_utc=start)
            rec.writer_args["extra"] = (lambda k=k, rec=rec: stream.slot_stats(k, rec))
            recs.append(rec)
        stream.recorders = recs
        # Receivers 2..N run in threads; receiver 1's Recorder owns the radio and takes Ctrl-C.
        results = [None] * len(recs)
        threads = []
        for k in range(1, len(recs)):
            t = threading.Thread(target=lambda k=k: results.__setitem__(k, recs[k].run()), daemon=True)
            t.start()
            threads.append(t)
        try:
            results[0] = recs[0].run()
        finally:
            for r in recs[1:]:
                if not r.done.wait(5.0):  # its last slot ends with receiver 1's; otherwise it is cut short
                    r.q.put(("abort",))
            for t in threads:
                t.join(timeout=5.0)
        done += min(len(r or []) for r in results)
        if stream.too_hot is not None:
            return f"HL2 at {stream.too_hot} C"
        if stream.lost:
            log(f"waiting {WATCHDOG_WAIT_S} s for the HL2's watchdog, then finding it again")
            time.sleep(WATCHDOG_WAIT_S)
            take = True  # a running report now is our own stream, not another client's
            continue
        if done < slots:
            return "stopped by Ctrl-C"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", required=True, help="this computer's address on the HL2's network")
    ap.add_argument("--host", help="the HL2's address; default: discover by broadcast")
    ap.add_argument("--dial-hz", type=int, help="one band: the dial, e.g. 7074000; the NCO goes here")
    ap.add_argument("--plan", help="DIAL[+DIAL...]:SLOTS[@HH:MM UTC],... each band in OUT/<band>")
    ap.add_argument("--out", required=True, help="output directory (never inside git)")
    ap.add_argument("--take", action="store_true", help="start even if the HL2 reports it is streaming")
    ap.add_argument("--max-temp-c", type=float, default=55.0, help="stop when the HL2 reports more (default 55)")
    n = ap.add_mutually_exclusive_group()
    n.add_argument("--slots", type=int, help="with --dial-hz: stop after this many slots")
    n.add_argument("--minutes", type=float, help="with --dial-hz: stop after this many minutes of slots")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.path.abspath(args.out).startswith(root + os.sep):
        ap.error("--out is inside the repository; recordings stay out of git")
    if bool(args.plan) == bool(args.dial_hz):
        ap.error("give --dial-hz with --slots or --minutes, or --plan")
    try:
        if args.plan:
            plan = parse_plan(args.plan)
        else:
            if args.slots is None and args.minutes is None:
                ap.error("--dial-hz needs --slots or --minutes")
            slots = args.slots if args.slots is not None else -(-int(args.minutes * 60) // qmx_record.SLOT_S)
            if slots < 1:
                ap.error("--slots/--minutes must give at least one slot")
            nco_register(args.dial_hz)
            plan = [([args.dial_hz], slots, None)]
    except ValueError as e:
        ap.error(str(e))

    single = not args.plan
    for dials, slots, when in plan:
        if when:
            start = next_utc(*when, time.time())
            if start > time.time():
                print(f"{' + '.join(band_name(d) for d in dials)} waits for {when[0]:02d}:{when[1]:02d} UTC "
                      f"({(start - time.time()) / 60:.0f} min)")
                time.sleep(start - time.time())
        if clock_synced() is False:
            sys.exit("the clock says it is not synced; slots are cut on UTC boundaries, so stopping")
        out_dirs = [args.out] if single else [os.path.join(args.out, band_name(d)) for d in dials]
        why = record_entry(args, dials, slots, out_dirs)
        if why:
            sys.exit(f"stopped at {' + '.join(band_name(d) for d in dials)}: {why}")
    print("plan done")


if __name__ == "__main__":
    main()
