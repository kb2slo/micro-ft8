#!/usr/bin/env python3
"""Host test of tools/hl2_pcap.py: a synthetic capture in, slot WAVs out, no radio needed.

    python3 tools/test_hl2_pcap.py
"""
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hl2_pcap  # noqa: E402
import hl2_record  # noqa: E402
import qmx_record  # noqa: E402

PER_FRAME = hl2_record.samples_per_frame(1)  # 63
PER_PACKET = 2 * PER_FRAME
T0 = 1_800_000_000.0  # a slot boundary (divisible by 15)


def rx_packet(sequence, first_sample):
    """An HL2 receive packet, one receiver: sample n has I = n and Q = -n, big-endian 24-bit."""
    out = bytearray(b"\xef\xfe\x01\x06" + struct.pack(">I", sequence))
    n = first_sample
    for _ in range(2):
        out += b"\x7f\x7f\x7f" + bytes([0x00, 0, 0, 0, 0])  # status address 0, nothing clipped
        for _ in range(PER_FRAME):
            out += (n & 0xFFFFFF).to_bytes(3, "big") + ((-n) & 0xFFFFFF).to_bytes(3, "big") + b"\x00\x00"
            n += 1
    assert len(out) == hl2_record.PACKET_LEN
    return bytes(out)


def frame(payload, sport=1024):
    udp = struct.pack(">HHHH", sport, 1025, 8 + len(payload), 0) + payload
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     bytes([169, 254, 1, 2]), bytes([169, 254, 3, 4]))
    return b"\x00" * 12 + b"\x08\x00" + ip + udp


def pcap(records, nanos=False):
    buf = io.BytesIO()
    buf.write(struct.pack("<IHHiIII", 0xA1B23C4D if nanos else 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
    for ts, data in records:
        sec = int(ts)
        frac = round((ts - sec) * (1e9 if nanos else 1e6))
        buf.write(struct.pack("<IIII", sec, frac, len(data), len(data)) + data)
    buf.seek(0)
    return buf


def stream(first_ts, packets, start_seq=0, skip=()):
    """(timestamp, frame) for consecutive packets, each stamped when its last sample would arrive."""
    out = []
    for k in range(packets):
        if k in skip:
            continue
        ts = first_ts + (k + 1) * PER_PACKET / qmx_record.SAMPLE_RATE
        out.append((ts, frame(rx_packet(start_seq + k, k * PER_PACKET))))
    return out


def run(records, nanos=False, slots=None, offset_s=0.0):
    out = tempfile.mkdtemp()
    CLEANUP.append(out)
    cutter = hl2_pcap.Cutter(out, 24915000, log=lambda *_: None, slots=slots, offset_s=offset_s)
    for ts, data, link in hl2_pcap.read_pcap(pcap(records, nanos)):
        u = hl2_pcap.udp_payload(data, link)
        if u is not None and u[0] == 1024:
            cutter.packet(ts, u[1])
    return out, cutter.close()


def first_iq(path):
    with wave.open(path) as w:
        raw = w.readframes(2)
    return (int.from_bytes(raw[0:3], "little", signed=True), int.from_bytes(raw[3:6], "little", signed=True))


CLEANUP = []
SLOT_PACKETS = qmx_record.FRAMES_PER_SLOT // PER_PACKET + 1


class Hl2PcapTest(unittest.TestCase):
    @classmethod
    def tearDownClass(cls):
        for d in CLEANUP:
            shutil.rmtree(d, ignore_errors=True)

    def test_starts_on_the_first_boundary(self):
        # Half a second of audio before the boundary, then two slots and a bit.
        lead = 0.5
        first = T0 - lead
        packets = int((2 * 15 + lead + 1) * qmx_record.SAMPLE_RATE / PER_PACKET)
        out, written = run(stream(first, packets))
        self.assertEqual(len(written), 2)
        self.assertEqual(written[0]["file"], qmx_record.slot_name(T0))
        self.assertEqual(written[1]["file"], qmx_record.slot_name(T0 + 15))
        i, q = first_iq(os.path.join(out, written[0]["file"]))
        self.assertEqual(i, round(lead * qmx_record.SAMPLE_RATE))  # the sample at the boundary
        self.assertEqual(q, -i)  # Q as sent: negation is the converter's job
        for rec in written:
            self.assertEqual(rec["frames"], qmx_record.FRAMES_PER_SLOT)
            self.assertEqual(rec["lost_packets"], 0)
            self.assertEqual(rec["overflows"], 0)
        with open(os.path.join(out, qmx_record.MANIFEST)) as f:
            manifest = [json.loads(l) for l in f]
        self.assertEqual([m["file"] for m in manifest], [r["file"] for r in written])

    def test_a_start_offset_moves_every_slot(self):
        lead = 0.5
        packets = int((2 * 15 + lead + 2) * qmx_record.SAMPLE_RATE / PER_PACKET)
        out, written = run(stream(T0 - lead, packets), offset_s=0.7)
        self.assertEqual(len(written), 2)
        self.assertEqual(written[0]["file"], qmx_record.slot_name(T0))  # named for its boundary
        self.assertEqual(written[0]["start_offset_s"], 0.7)
        i, _q = first_iq(os.path.join(out, written[0]["file"]))
        self.assertEqual(i, round((lead + 0.7) * qmx_record.SAMPLE_RATE))
        # The second slot follows the first with no frame dropped or repeated.
        i2, _q = first_iq(os.path.join(out, written[1]["file"]))
        self.assertEqual(i2, i + qmx_record.FRAMES_PER_SLOT)

    def test_since_and_until_take_the_slots_between(self):
        # Four slots of stream; keep the middle two, as for a capture retuned at both ends.
        packets = int((4 * 15 + 1) * qmx_record.SAMPLE_RATE / PER_PACKET)
        recs = stream(T0, packets)
        out = tempfile.mkdtemp()
        CLEANUP.append(out)
        cutter = hl2_pcap.Cutter(out, 14074000, log=lambda *_: None)
        written = hl2_pcap.cut_capture(pcap(recs), cutter, since=T0 + 15, until=T0 + 45)
        self.assertEqual([r["file"] for r in written], [qmx_record.slot_name(T0 + 15), qmx_record.slot_name(T0 + 30)])
        i, _q = first_iq(os.path.join(out, written[0]["file"]))
        self.assertEqual(i, 15 * qmx_record.SAMPLE_RATE)  # the stream's sample at the --since boundary
        self.assertEqual([f for f in os.listdir(out) if f.endswith(".part")], [])

    def test_a_gap_is_zero_filled_and_counted(self):
        packets = SLOT_PACKETS + 50
        out, written = run(stream(T0, packets, skip={100, 101}))
        self.assertEqual(len(written), 1)
        self.assertEqual(written[0]["frames"], qmx_record.FRAMES_PER_SLOT)
        self.assertEqual(written[0]["lost_packets"], 2)
        self.assertEqual(written[0]["overflows"], 1)
        with wave.open(os.path.join(out, written[0]["file"])) as w:
            w.readframes(100 * PER_PACKET)
            hole = w.readframes(2 * PER_PACKET)
            after = w.readframes(1)
        self.assertEqual(hole, bytes(len(hole)))
        # The packet after the gap lands where it was sent: the timeline holds.
        self.assertEqual(int.from_bytes(after[0:3], "little", signed=True), 102 * PER_PACKET)

    def test_a_late_packet_is_dropped(self):
        recs = stream(T0, SLOT_PACKETS + 10)
        recs.insert(60, recs[40])  # packet 40 again, after 59
        _out, written = run(recs)
        self.assertEqual(written[0]["late_packets"], 1)
        self.assertEqual(written[0]["lost_packets"], 0)

    def test_nanosecond_capture_and_other_traffic(self):
        recs = stream(T0, SLOT_PACKETS + 10)
        recs.insert(5, (recs[5][0], frame(b"\x00" * 64, sport=1025)))  # the P4's control packets
        _out, written = run(recs, nanos=True)
        self.assertEqual(len(written), 1)
        self.assertEqual(written[0]["lost_packets"], 0)

    def test_an_unfinished_slot_is_dropped(self):
        out, written = run(stream(T0, SLOT_PACKETS // 2))
        self.assertEqual(written, [])
        self.assertEqual([f for f in os.listdir(out) if f.endswith(".part")], [])

    def test_not_pcap(self):
        with self.assertRaises(ValueError):
            list(hl2_pcap.read_pcap(io.BytesIO(b"\x0a\x0d\x0d\x0a" + bytes(40))))


if __name__ == "__main__":
    unittest.main()
