#!/usr/bin/env python3
"""Host test for tools/hl2_record.py, no radio needed.

Holds its wire format to the golden frames host_mock/test_hl2_proto.cpp holds the C codec to, so the two
cannot drift apart, and checks the stream's gap fill, LNA stepping and per-slot counts.

  python3 tools/test_hl2_record.py
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hl2_record as h  # noqa: E402

GOLDEN = os.path.join(os.path.dirname(HERE), "host_mock", "hl2_golden")


def load_hex(name):
    out = bytearray()
    with open(os.path.join(GOLDEN, name), encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                out += bytes.fromhex(line)
    return bytes(out)


def test_control_packet():
    golden = load_hex("ep2_rx_config_seq0.hex")
    pkt = h.control_packet(0, h.config_register(0), h.nco_register(14074000))
    assert pkt == golden, "byte for byte the packet the HL2 streamed on"
    for f in (8, 8 + h.FRAME_LEN):
        assert pkt[f + 3] & 0x01 == 0 and not any(pkt[f + 8:f + h.FRAME_LEN]), "MOX clear, no transmit samples"
    for bad in ((0x09, 0), (0x17, 0), (h.REG_CONFIG, 1 << 16)):
        try:
            h.control_packet(0, bad, h.nco_register(7074000))
        except ValueError:
            continue
        raise AssertionError(f"register write {bad} must be refused")


def test_registers():
    assert h.n2adr_receive_filters(7074000) == 0x04, "40 m: the 60/40 m low-pass, the high-pass in line"
    assert h.n2adr_receive_filters(14074000) == 0x08 and h.n2adr_receive_filters(28074000) == 0x20
    assert h.n2adr_receive_filters(1840000) == 0x41, "160 m: its low-pass and the high-pass bypassed"
    assert h.n2adr_receive_filters(3573000) == 0x02 and h.n2adr_receive_filters(21074000) == 0x10
    assert h.lna_register(20) == (h.REG_LNA, 0x40 | 32) and h.lna_register(-12) == (h.REG_LNA, 0x40)
    for bad in (-13, 49):
        try:
            h.lna_register(bad)
        except ValueError:
            continue
        raise AssertionError(f"LNA {bad} dB must be refused")


def test_discovery():
    r = h.parse_discovery_reply(load_hex("discovery_reply_2026-11-03.hex"))
    assert r and not r["running"] and r["gateware"] == "74.2" and r["receivers"] == 4
    assert r["temperature_c"] == 25.5
    assert h.parse_discovery_reply(load_hex("discovery_reply_2026-11-03.hex")[:59]) is None
    assert len(h.discovery_request()) == 63 and len(h.start_packet()) == 64 and h.stop_packet()[3] == 0


def test_rx_packet():
    seq, pcm, status = h.parse_rx_packet(load_hex("ep6_1rx_48k_2026-11-03.hex"))
    v = h.sample_values(pcm[0])
    assert seq == 100 and len(pcm) == 1 and len(v) == 2 * 126, "sequence 100, one receiver, 126 I/Q samples"
    assert v[0:4] == [72, -106, 29, -28], "the first samples, sign-extended, as the C codec reads them"
    assert v[124:128] == [-11, -82, 1, -7] and v[250:252] == [22, -34], "frame 1's last, frame 2's first and last"
    assert [a for a, _ in status] == [3, 0], "frame 1 status address 3, frame 2 address 0"
    assert status[1][1][0] & 0x01 == 0, "no clip in the capture"


def four_rx_packet(seq):
    """A synthetic 4-receiver packet: receiver r's I at sample n is 1000 r + n, its Q the negative."""
    p = bytearray(h.PACKET_LEN)
    p[0:8] = b"\xef\xfe\x01\x06" + struct.pack(">I", seq)
    n = 0
    for f in (8, 8 + h.FRAME_LEN):
        p[f:f + 4] = b"\x7f\x7f\x7f\x00"
        s = f + 8
        for _ in range(19):
            for r in range(4):
                p[s + 6 * r:s + 6 * r + 3] = (1000 * r + n).to_bytes(3, "big", signed=True)
                p[s + 6 * r + 3:s + 6 * r + 6] = (-(1000 * r + n)).to_bytes(3, "big", signed=True)
            s += 26
            n += 1
    return bytes(p)


def test_four_receivers():
    assert h.samples_per_frame(1) == 63 and h.samples_per_frame(4) == 19
    assert h.config_register(0, 4) == (h.REG_CONFIG, (3 << 3) | (1 << 2)), "four receivers: last receiver 3, duplex"
    assert h.nco_register(7074000, 3) == (0x05, 7074000), "receiver 4's NCO is register 0x05"
    seq, pcm, _ = h.parse_rx_packet(four_rx_packet(5), 4)
    assert len(pcm) == 4 and all(len(x) == 38 * 6 for x in pcm), "38 samples a packet for each of four"
    for r in range(4):
        v = h.sample_values(pcm[r])
        assert v[0:2] == [1000 * r, -1000 * r] and v[74:76] == [1000 * r + 37, -(1000 * r + 37)]
    sock = FakeSock()
    s = h.Hl2Stream(sock, ("169.254.0.2", h.PORT), [28074000, 21074000, 14074000, 7074000], log=lambda *_: None)
    assert s.n2adr == 0x20, "four bands share the highest band's low-pass, the high-pass in line"
    for i in range(8):
        s.handle(four_rx_packet(i), i * 0.01)
    ncos = {struct.unpack(">I", p[8 + h.FRAME_LEN + 4:8 + h.FRAME_LEN + 8])[0] for p in sock.sent}
    assert ncos == {28074000, 21074000, 14074000, 7074000}, "every receiver's NCO goes out in turn"
    assert h.Hl2Stream(FakeSock(), ("x", 1), [1840000, 3573000], log=lambda *_: None).n2adr == 0x02, \
        "160 and 80 m: 80 m's low-pass, and the high-pass in line, since not every band is 160 m"


class FakeSock:
    def __init__(self):
        self.sent = []

    def sendto(self, d, addr):
        self.sent.append(d)


def packet(seq, clipped=False):
    p = bytearray(load_hex("ep6_1rx_48k_2026-11-03.hex"))
    p[4:8] = struct.pack(">I", seq)
    if clipped:
        p[8 + h.FRAME_LEN + 4] |= 0x01  # frame 2 is status address 0: C1[0], the ADC clipped
    return bytes(p)


class FakeWriter:
    index = 0


class FakeRecorder:
    def __init__(self):
        import queue
        self.q = queue.Queue()
        self.writer = FakeWriter()


def test_stream():
    sock = FakeSock()
    s = h.Hl2Stream(sock, ("169.254.0.2", h.PORT), [7074000], log=lambda *_: None, start_utc=0.0)
    frames, gap = s.handle(packet(10), 0.0)
    assert len(frames) == 1 and len(frames[0]) == 126 * 6 and not gap, "the first packet: its 126 frames, no gap"
    frames, gap = s.handle(packet(13), 0.1)
    assert len(frames[0]) == 3 * 126 * 6 and gap and not any(frames[0][:2 * 126 * 6]), "two lost packets zero-filled"
    assert s.handle(packet(12), 0.2) is None, "a late packet: dropped, its place already zero-filled"
    assert s.handle(b"\x00" * 10, 0.3) is None, "not a receive packet: skipped"
    for i in range(20):  # a clipping second, then the check
        s.handle(packet(14 + i, clipped=True), 0.4 + i * 0.05)
    assert s.lna_db == 17, "clipping past 1% of a second's status words steps the LNA down 3 dB"
    rec = FakeRecorder()
    st = s.slot_stats(0, rec)
    assert st["lost_packets"] == 2 and st["late_packets"] == 1, "no false gap after the late packet"
    assert st["clipped_status_words"] == 20 and st["status_words"] == 22 and st["receiver"] == 1
    assert st["lna_db_start"] == 20 and st["lna_db_end"] == 17 and st["radio"] == "HL2" and st["n2adr"] == 0x04
    s.handle(packet(40), 15.5)
    rec.writer.index = 1
    assert s.slot_stats(0, rec)["status_words"] == 1, "the next slot counts from zero"
    assert all(len(p) == h.PACKET_LEN and p[3] == 0x02 for p in sock.sent), "one control packet per packet"
    assert all(p[11] & 0x01 == 0 and p[11 + h.FRAME_LEN] & 0x01 == 0 for p in sock.sent), "MOX always clear"


def test_temperature_and_abort():
    s = h.Hl2Stream(FakeSock(), ("169.254.0.2", h.PORT), [14074000], log=lambda *_: None, max_temp_c=30.0,
                    start_utc=0.0)
    recs = [FakeRecorder(), FakeRecorder()]
    s.recorders = recs
    p = bytearray(packet(1))
    p[8 + 3] = (1 << 3)  # frame 1 carries status address 1: C1..C2 the temperature code
    p[8 + 4:8 + 6] = struct.pack(">H", 0x3B4)  # 25.5 C
    s.handle(bytes(p), 0.0)
    assert s.stats[0]["temp_c_min"] == s.stats[0]["temp_c_max"] == 25.5 and s.too_hot is None
    p[4:8] = struct.pack(">I", 2)
    p[8 + 4:8 + 6] = struct.pack(">H", 0x500)  # 51.9 C, over the 30 C limit
    s.handle(bytes(p), 0.1)
    assert s.too_hot == 51.9 and all(r.q.get_nowait() == ("abort",) for r in recs), "over the limit: every receiver stops"
    assert s.stats[0]["temp_c_max"] == 51.9


def test_plan():
    assert h.parse_plan("28074000:120, 7074000:60@00:00") == [([28074000], 120, None), ([7074000], 60, (0, 0))]
    assert h.parse_plan("28074000+21074000+14074000+7074000:120") == [
        ([28074000, 21074000, 14074000, 7074000], 120, None)]
    for bad in ("28074000", "50313000:10", "7074000:0", "7074000:5@25:00", "7074000+7076000:5",
                "1840000+3573000+7074000+14074000+28074000:5"):
        try:
            h.parse_plan(bad)
        except ValueError:
            continue
        raise AssertionError(f"plan {bad!r} must be refused")
    assert h.band_name(7074000) == "40m" and h.band_name(1840000) == "160m" and h.band_name(28074000) == "10m"
    now = 1762300800.0  # 2025-11-05 00:00 UTC
    assert h.next_utc(0, 0, now) == now and h.next_utc(1, 0, now) == now + 3600
    assert h.next_utc(23, 0, now + 3600) == now - 3600, "an hour already past is not waited for"


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("OK")
