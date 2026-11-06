#!/usr/bin/env python3
"""Turn a capture of an HL2's stream into the slot WAVs tools/hl2_record.py writes.

The P4 decodes the HL2 while this computer, bridged into the cable between them (or on a switch's
mirror port), captures the same packets; this file cuts that capture into one 48 kHz I/Q WAV per 15 s
UTC slot, so the P4's decodes and the host's are compared on the same slots (docs/architecture/hl2.md,
**A capture beside the P4**). It only reads: nothing is sent to the radio, which the P4 drives.

Capture with any tool that writes pcap, only the HL2's packets to the P4 (Protocol 1 sends from
UDP port 1024), on a computer whose clock is synced to well under a second: slots are cut on UTC
boundaries by the capture's own timestamps.

  sudo tcpdump -i br-hl2 -w hl2.pcap udp src port 1024
  python3 tools/hl2_pcap.py --dial-hz 24915000 --out DIR hl2.pcap

--start-offset-s S starts each slot S seconds after its boundary. A main board's decode sync moves its
clock until signals start near 0 on it, about 0.5 s behind UTC (docs/architecture/time.md, **Where it
settles**), so a cut on UTC itself ends its decode window half a second before the signals do.

The WAVs and manifest.jsonl are hl2_record.py's: I on the left as sent, Q on the right as sent (the
HL2's Q is negated, so convert with --negate-q), a sequence gap zero-filled with that many packets'
samples and counted as an overflow, the audio before the first whole slot dropped. Each manifest
line also carries the packets lost and late in that slot. Classic pcap only, microsecond or
nanosecond timestamps, Ethernet or Linux cooked framing; pcapng: convert first
(`editcap -F pcap in.pcapng out.pcap`, part of Wireshark).

Python 3 only, no packages. Host test: python3 tools/test_hl2_pcap.py
"""
import argparse
import calendar
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hl2_record  # noqa: E402  the HL2's wire format
import qmx_record  # noqa: E402  the slot cutting and manifest

LINKTYPE_ETHERNET = 1
LINKTYPE_LINUX_SLL = 113
MAGIC = {0xA1B2C3D4: ("<", 1e-6), 0xD4C3B2A1: (">", 1e-6), 0xA1B23C4D: ("<", 1e-9), 0x4D3CB2A1: (">", 1e-9)}


def read_pcap(f):
    """Yields (timestamp, frame bytes, link type) for each record of a classic pcap file."""
    header = f.read(24)
    if len(header) < 24:
        raise ValueError("not a pcap file: too short")
    magic = struct.unpack("<I", header[:4])[0]
    if magic not in MAGIC:
        raise ValueError("not a classic pcap file (pcapng? convert it with editcap -F pcap)")
    order, unit = MAGIC[magic]
    link = struct.unpack(order + "I", header[20:24])[0]
    while True:
        rec = f.read(16)
        if len(rec) < 16:
            return
        sec, frac, incl, _orig = struct.unpack(order + "IIII", rec)
        data = f.read(incl)
        if len(data) < incl:
            return  # a capture cut off mid-record: its last packet is lost
        yield sec + frac * unit, data, link


def udp_payload(frame, link):
    """(source port, payload) of an IPv4 UDP frame, or None."""
    if link == LINKTYPE_ETHERNET:
        off, ethertype = 14, frame[12:14]
        if ethertype == b"\x81\x00":  # one VLAN tag
            off, ethertype = 18, frame[16:18]
    elif link == LINKTYPE_LINUX_SLL:
        off, ethertype = 16, frame[14:16]
    else:
        raise ValueError(f"link type {link}: only Ethernet and Linux cooked captures")
    if ethertype != b"\x08\x00" or len(frame) < off + 20:
        return None
    ihl = (frame[off] & 0x0F) * 4
    if frame[off + 9] != 17:  # UDP
        return None
    u = off + ihl
    if len(frame) < u + 8:
        return None
    sport, _dport, length = struct.unpack(">HHH", frame[u:u + 6])
    return sport, frame[u + 8:u + length]


class Cutter:
    """Feeds one receiver's samples into a SlotWriter from the first UTC boundary on.

    A packet's samples are taken to end at its timestamp, which a capture stamps on arrival; the first
    boundary after the first packet is where slot 0 starts, and the samples before it are dropped.
    """

    def __init__(self, out_dir, dial_hz, receivers=1, log=print, version="unknown", slots=None, offset_s=0.0):
        self.out_dir, self.dial_hz, self.receivers, self.log = out_dir, dial_hz, receivers, log
        self.offset_s = offset_s  # each slot starts this long after its UTC boundary
        self.version, self.slots = version, slots
        self.fill = bytes(hl2_record.samples_per_frame(receivers) * 2 * qmx_record.FRAME_BYTES)
        self.writer = None
        self.skip = None  # frames still to drop before slot 0
        self.expected = None
        self.lost = self.late = 0  # this slot's
        self.packets = 0

    def _stats(self):
        st = {"radio": "HL2", "receivers": self.receivers, "source": "pcap", "start_offset_s": self.offset_s,
              "lost_packets": self.lost, "late_packets": self.late}
        self.lost = self.late = 0
        return st

    def packet(self, ts, payload):
        p = hl2_record.parse_rx_packet(payload, self.receivers)
        if p is None:
            return
        sequence, pcm, _status = p
        self.packets += 1
        missing = 0 if self.expected is None else (sequence - self.expected) & hl2_record.SEQ_MASK
        if missing > hl2_record.SEQ_MASK >> 1:
            self.late += 1  # a duplicate or late packet: its place is already written
            return
        if missing > hl2_record.MAX_FILL_PACKETS:
            missing = 0  # a restart: no fill, as hl2_record does
        self.expected = (sequence + 1) & hl2_record.SEQ_MASK
        data = self.fill * missing + pcm[0]
        frames = len(data) // qmx_record.FRAME_BYTES
        if self.writer is None:
            first = ts - frames / qmx_record.SAMPLE_RATE
            start = qmx_record.next_boundary(first - self.offset_s) + self.offset_s
            self.skip = round((start - first) * qmx_record.SAMPLE_RATE)
            os.makedirs(self.out_dir, exist_ok=True)
            self.writer = qmx_record.SlotWriter(self.out_dir, start, self.slots, "iq", dial_hz=self.dial_hz,
                                                version=self.version, log=self.log, extra=self._stats)
        if self.skip:
            drop = min(self.skip, frames)
            self.skip -= drop
            data = data[drop * qmx_record.FRAME_BYTES:]
            if not data:
                return
        self.lost += missing
        self.writer.feed(data, missing > 0, ts)

    def close(self):
        """Drops the unfinished last slot; returns the manifest records of the slots written."""
        if self.writer is None:
            return []
        if not self.writer.finished:
            self.writer.abort()
        return self.writer.written


# A packet is stamped when its last sample arrives, so the one that completes a slot ending at --until is
# stamped just after it: a packet is 2.6 ms of audio at one receiver.
UNTIL_MARGIN_S = 0.1


def cut_capture(f, cutter, port=hl2_record.PORT, since=None, until=None):
    """Feeds a pcap's HL2 packets from since to until (epoch seconds, or None) through cutter; returns the
    manifest records of the slots written."""
    for ts, frame, link in read_pcap(f):
        if since is not None and ts < since:
            continue
        if until is not None and ts > until + UNTIL_MARGIN_S:
            break  # a slot still open here ends past until; close() drops it
        u = udp_payload(frame, link)
        if u is None or u[0] != port:
            continue
        cutter.packet(ts, u[1])
        if cutter.writer is not None and cutter.writer.finished:
            break
    return cutter.close()


def utc_seconds(text):
    return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%S"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pcap")
    ap.add_argument("--out", required=True, help="directory for the slot WAVs and manifest.jsonl")
    ap.add_argument("--dial-hz", type=int, required=True, help="the receiver's dial, recorded as given")
    ap.add_argument("--receivers", type=int, default=1, help="receivers in the stream (the P4 runs one)")
    ap.add_argument("--port", type=int, default=hl2_record.PORT, help="the HL2's UDP source port")
    ap.add_argument("--slots", type=int, default=None, help="stop after this many slots")
    ap.add_argument("--since", help="UTC, YYYY-MM-DDTHH:MM:SS: the first slot starts at or after this; packets "
                                    "before it are skipped (a capture whose radio was retuned partway)")
    ap.add_argument("--until", help="UTC, YYYY-MM-DDTHH:MM:SS: the last slot ends at or before this")
    ap.add_argument("--start-offset-s", type=float, default=0.0,
                    help="start each slot this long after its UTC boundary, as a main board whose clock its "
                         "decode sync moved does (docs/architecture/time.md); the file keeps the boundary's name")
    args = ap.parse_args()
    if not 0.0 <= args.start_offset_s < 1.0:
        ap.error("--start-offset-s is 0 to under 1 s: a file is named for its slot's boundary")
    since = utc_seconds(args.since) if args.since else None
    until = utc_seconds(args.until) if args.until else None

    cutter = Cutter(args.out, args.dial_hz, args.receivers, version=qmx_record.recorder_version(),
                    slots=args.slots, log=lambda *_: None, offset_s=args.start_offset_s)
    with open(args.pcap, "rb") as f:
        written = cut_capture(f, cutter, args.port, since, until)
    lost = sum(r["lost_packets"] for r in written)
    print(f"{len(written)} slots from {cutter.packets} packets into {args.out}; packets lost {lost}")
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
