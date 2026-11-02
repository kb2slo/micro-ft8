#!/usr/bin/env python3
"""Host test for tools/qmx_record.py, with a fake input stream: no PortAudio device needed.

  python3 tools/test_qmx_record.py
"""
import hashlib
import json
import os
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmx_record as qr  # noqa: E402

FB = qr.FRAME_BYTES
T0 = 1_793_620_800.0  # 2026-11-02T12:00:00Z, a slot boundary
# Odd block sizes, none dividing 720,000, so cuts fall mid-block.
BLOCKS = [1, 7, 997, 4093, 511, 3001, 12_347]


def frames(start, n):
    """n frames numbered from start: each sample holds its frame number (L) or its complement (R), 24-bit LE."""
    out = bytearray()
    for i in range(start, start + n):
        v = i & 0xFFFFFF
        out += v.to_bytes(3, "little") + (v ^ 0xFFFFFF).to_bytes(3, "little")
    return bytes(out)


class FakeCat:
    def __init__(self, q9="Q9 1;", dial=14_074_000):
        self.q9, self.dial, self.reads = q9, dial, 0

    def read_q9(self):
        self.reads += 1
        return self.q9

    def read_dial_hz(self):
        return self.dial


class FakeStatus:
    def __init__(self, overflow):
        self.input_overflow = overflow

    def __bool__(self):
        return self.input_overflow


class FakeStream:
    """Delivers numbered frames to the callback in odd-sized blocks, advancing a fake clock."""

    def __init__(self, clock, overflow_at=()):
        self.clock = clock
        self.overflow_at = set(overflow_at)  # block indices flagged as overflowed
        self.callback = None
        self.next_frame = 0
        self.block = 0
        self.started = self.stopped = self.closed = False

    def open(self, callback):
        self.callback = callback
        return self

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def close(self):
        self.closed = True

    def pump(self, n_frames):
        """Deliver n_frames frames (in BLOCKS-sized pieces); returns the bytes delivered."""
        sent = bytearray()
        while n_frames > 0:
            n = min(BLOCKS[self.block % len(BLOCKS)], n_frames)
            data = frames(self.next_frame, n)
            self.clock.t += n / qr.SAMPLE_RATE  # arrival time is the block's last frame
            self.callback(data, n, None, FakeStatus(self.block in self.overflow_at))
            self.next_frame += n
            self.block += 1
            n_frames -= n
            sent += data
        return bytes(sent)


class Clock:
    def __init__(self, t):
        self.t = t

    def __call__(self):
        return self.t


def read_wav(path):
    with wave.open(path, "rb") as w:
        return (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes(),
                w.readframes(w.getnframes()))


class AlternateTest(unittest.TestCase):
    def test_turns_of_n_slots_starting_with_the_first_tag(self):
        self.assertEqual([qr.mode_for(i, "iq", 2) for i in range(6)],
                         ["iq", "iq", "audio", "audio", "iq", "iq"])
        self.assertEqual([qr.mode_for(i, "audio", 1) for i in range(3)], ["audio", "iq", "audio"])
        self.assertEqual([qr.mode_for(i, "iq", None) for i in range(3)], ["iq"] * 3)

    def test_switch_lands_before_each_turn_boundary(self):
        plan = qr.switch_plan(T0, 5, "iq", 2)
        self.assertEqual(plan, [(T0 - qr.SWITCH_LEAD_S, "iq"),
                                (T0 + 2 * qr.SLOT_S - qr.SWITCH_LEAD_S, "audio"),
                                (T0 + 4 * qr.SLOT_S - qr.SWITCH_LEAD_S, "iq")])
        self.assertEqual(qr.switch_plan(T0, 5, "iq", None), [])


class SlotWriterTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.logs = []

    def tearDown(self):
        self.tmp.cleanup()

    def writer(self, **kw):
        args = dict(out_dir=self.dir, first_start=T0, slots=3, mode_tag="iq", cat=FakeCat(),
                    dial_hz=None, version="test-1", log=self.logs.append)
        args.update(kw)
        return qr.SlotWriter(**args)

    def feed_all(self, w, total, overflow_blocks=()):
        sent = bytearray()
        start, b, t = 0, 0, T0
        while start < total and not w.finished:
            n = min(BLOCKS[b % len(BLOCKS)], total - start)
            data = frames(start, n)
            t += n / qr.SAMPLE_RATE
            w.feed(data, b in overflow_blocks, t)
            sent += data
            start += n
            b += 1
        return bytes(sent)

    def test_cuts_every_720000_frames_with_no_frame_lost_or_duplicated(self):
        w = self.writer()
        sent = self.feed_all(w, 3 * qr.FRAMES_PER_SLOT + 5000)
        self.assertTrue(w.finished)
        names = ["261102_120000.wav", "261102_120015.wav", "261102_120030.wav"]
        self.assertEqual(sorted(f for f in os.listdir(self.dir) if f.endswith(".wav")), names)
        self.assertFalse([f for f in os.listdir(self.dir) if f.endswith(".part")])
        joined = b"".join(read_wav(os.path.join(self.dir, n))[4] for n in names)
        self.assertEqual(joined, sent[:3 * qr.FRAMES_PER_SLOT * FB])
        # Each file starts on the frame after the last one of the file before it.
        for k, n in enumerate(names):
            first = read_wav(os.path.join(self.dir, n))[4][:3]
            self.assertEqual(int.from_bytes(first, "little"), k * qr.FRAMES_PER_SLOT)

    def test_alternate_tags_each_slot_and_warns_on_a_mode_mismatch(self):
        w = self.writer(slots=4, alternate=2, cat=FakeCat(q9="Q9 1;"))
        self.feed_all(w, 4 * qr.FRAMES_PER_SLOT)
        self.assertEqual([r["mode_tag"] for r in w.written], ["iq", "iq", "audio", "audio"])
        warned = [m for m in self.logs if m.startswith("warning: Q9;")]
        self.assertEqual(len(warned), 2)  # the two audio slots, while the fake radio stays in IQ

    def test_24_bit_wav_headers(self):
        w = self.writer(slots=1)
        self.feed_all(w, qr.FRAMES_PER_SLOT)
        path = os.path.join(self.dir, "261102_120000.wav")
        ch, width, rate, n, _ = read_wav(path)
        self.assertEqual((ch, width, rate, n), (2, 3, 48000, 720_000))
        with open(path, "rb") as f:
            head = f.read(44)
        self.assertEqual(head[:4], b"RIFF")
        self.assertEqual(head[8:16], b"WAVEfmt ")
        self.assertEqual(int.from_bytes(head[20:22], "little"), 1)  # PCM
        self.assertEqual(int.from_bytes(head[32:34], "little"), 6)  # block align
        self.assertEqual(int.from_bytes(head[34:36], "little"), 24)  # bits per sample
        self.assertEqual(int.from_bytes(head[40:44], "little"), 720_000 * 6)
        self.assertEqual(os.path.getsize(path), 44 + 720_000 * 6)

    def test_manifest_fields(self):
        w = self.writer(slots=2)
        self.feed_all(w, 2 * qr.FRAMES_PER_SLOT)
        with open(os.path.join(self.dir, qr.MANIFEST)) as f:
            recs = [json.loads(line) for line in f]
        self.assertEqual(len(recs), 2)
        r = recs[1]
        self.assertEqual(set(r), {
            "file", "nominal_utc_start", "first_frame_arrived_utc", "mode_tag", "q9_reply", "dial_hz",
            "dial_source", "sample_rate", "channels", "sample_width_bytes", "frames", "overflows",
            "sha256", "recorder"})
        self.assertEqual(r["file"], "261102_120015.wav")
        self.assertEqual(r["nominal_utc_start"], "2026-11-02T12:00:15.000+00:00")
        self.assertTrue(r["first_frame_arrived_utc"].startswith("2026-11-02T12:00:15"))
        self.assertEqual((r["mode_tag"], r["q9_reply"], r["dial_hz"], r["dial_source"]),
                         ("iq", "Q9 1;", 14_074_000, "cat"))
        self.assertEqual((r["sample_rate"], r["channels"], r["sample_width_bytes"], r["frames"]),
                         (48000, 2, 3, 720_000))
        self.assertEqual(r["recorder"], "test-1")
        with open(os.path.join(self.dir, r["file"]), "rb") as f:
            self.assertEqual(r["sha256"], hashlib.sha256(f.read()).hexdigest())

    def test_dial_given_without_cat_and_fallback_when_cat_is_silent(self):
        w = self.writer(slots=1, cat=None, dial_hz=7_074_000)
        self.feed_all(w, qr.FRAMES_PER_SLOT)
        r = w.written[0]
        self.assertEqual((r["dial_hz"], r["dial_source"], r["q9_reply"]), (7_074_000, "given", None))
        os.remove(os.path.join(self.dir, r["file"]))
        w = self.writer(slots=1, cat=FakeCat(dial=None), dial_hz=7_074_000)
        self.feed_all(w, qr.FRAMES_PER_SLOT)
        self.assertEqual((w.written[0]["dial_hz"], w.written[0]["dial_source"]), (7_074_000, "given"))

    def test_q9_read_once_per_slot_and_mismatch_warned(self):
        cat = FakeCat(q9="Q9 0;")
        w = self.writer(slots=2, cat=cat, mode_tag="iq")
        self.feed_all(w, 2 * qr.FRAMES_PER_SLOT)
        self.assertEqual(cat.reads, 2)
        self.assertEqual(sum("Q9; answered" in m for m in self.logs), 2)
        self.logs.clear()
        for f in os.listdir(self.dir):
            os.remove(os.path.join(self.dir, f))
        w = self.writer(slots=1, cat=FakeCat(q9="?;"), mode_tag="audio")
        self.feed_all(w, qr.FRAMES_PER_SLOT)
        self.assertEqual(w.written[0]["q9_reply"], "?;")  # unknown reply: stored, not warned
        self.assertFalse([m for m in self.logs if "warning" in m])

    def test_overflows_counted_per_slot(self):
        # Block 0 lands in slot 0; find blocks whose first frame lands in slot 1.
        starts, s = [], 0
        for b in range(2000):
            starts.append(s)
            s += BLOCKS[b % len(BLOCKS)]
        in_slot1 = [b for b, s in enumerate(starts) if qr.FRAMES_PER_SLOT <= s < 2 * qr.FRAMES_PER_SLOT]
        straddler = next(b for b, s in enumerate(starts)
                         if s < qr.FRAMES_PER_SLOT < s + BLOCKS[b % len(BLOCKS)])
        w = self.writer(slots=2)
        self.feed_all(w, 2 * qr.FRAMES_PER_SLOT, overflow_blocks={0, straddler, in_slot1[3], in_slot1[9]})
        self.assertEqual([r["overflows"] for r in w.written], [2, 2])

    def test_stop_mid_slot_finishes_the_slot_then_stops(self):
        w = self.writer(slots=None)
        w.feed(frames(0, 1000), False, T0)
        w.request_stop()
        self.assertFalse(w.finished)
        w.feed(frames(1000, qr.FRAMES_PER_SLOT), False, T0 + 15)
        self.assertTrue(w.finished)
        self.assertEqual([r["frames"] for r in w.written], [qr.FRAMES_PER_SLOT])
        self.assertEqual(sorted(os.listdir(self.dir)), ["261102_120000.wav", qr.MANIFEST])

    def test_abort_mid_slot_leaves_nothing_partial(self):
        w = self.writer(slots=None)
        w.feed(frames(0, qr.FRAMES_PER_SLOT + 1234), False, T0 + 15)
        w.abort()
        self.assertTrue(w.finished)
        self.assertEqual(sorted(os.listdir(self.dir)), ["261102_120000.wav", qr.MANIFEST])
        with open(os.path.join(self.dir, qr.MANIFEST)) as f:
            self.assertEqual(len(f.readlines()), 1)

    def test_refuses_to_overwrite(self):
        open(os.path.join(self.dir, "261102_120000.wav"), "wb").close()
        w = self.writer(slots=1)
        with self.assertRaises(FileExistsError):
            w.feed(frames(0, 10), False, T0)


class RecorderTest(unittest.TestCase):
    """The whole path: callback queues, writer thread writes, Ctrl-C arrives in the main wait."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.logs = []

    def tearDown(self):
        self.tmp.cleanup()

    def recorder(self, plan, slots=None, overflow_at=()):
        clock = Clock(T0 - 3.2)  # the first boundary is T0 (at least LEAD_S ahead)
        stream = FakeStream(clock, overflow_at)
        steps = iter(plan)
        self.sent = bytearray()

        def wait(done):
            if done.wait(0.01):
                return
            step = next(steps, None)
            if step is None:
                if not done.wait(5):  # the plan ran out: fail, do not hang
                    raise AssertionError("the recorder did not finish on its plan")
            elif step == "ctrl-c":
                raise KeyboardInterrupt
            else:
                self.sent += stream.pump(step)

        rec = qr.Recorder(stream.open, dict(out_dir=self.dir, slots=slots, mode_tag="audio",
                                            cat=FakeCat(q9="Q9 0;"), version="t", log=self.logs.append),
                          clock=clock, wait=wait)
        return rec, stream

    def test_starts_on_the_boundary_and_trims_the_straddling_block(self):
        pre = round(3.2 * qr.SAMPLE_RATE)  # frames delivered before T0
        rec, stream = self.recorder([pre + 20_000, qr.FRAMES_PER_SLOT], slots=1)
        written = rec.run()
        self.assertEqual(rec.start_utc, T0)
        self.assertEqual(len(written), 1)
        self.assertTrue(stream.stopped and stream.closed)
        data = read_wav(os.path.join(self.dir, written[0]["file"]))[4]
        first = int.from_bytes(data[:3], "little")
        self.assertLessEqual(abs(first - pre), 1)  # the frame captured at T0, to a frame
        # Continuous from there: exactly the stream's frames, in order.
        self.assertEqual(data, bytes(self.sent[first * FB:(first + qr.FRAMES_PER_SLOT) * FB]))

    def test_ctrl_c_mid_slot_finishes_the_slot(self):
        pre = round(3.2 * qr.SAMPLE_RATE)
        plan = [pre + qr.FRAMES_PER_SLOT + 100_000, "ctrl-c", qr.FRAMES_PER_SLOT]
        rec, _ = self.recorder(plan)
        written = rec.run()
        self.assertEqual([r["frames"] for r in written], [qr.FRAMES_PER_SLOT] * 2)
        self.assertFalse([f for f in os.listdir(self.dir) if f.endswith(".part")])
        self.assertTrue(any("Ctrl-C" in m for m in self.logs))

    def test_second_ctrl_c_stops_now_with_nothing_partial(self):
        pre = round(3.2 * qr.SAMPLE_RATE)
        rec, _ = self.recorder([pre + qr.FRAMES_PER_SLOT + 100_000, "ctrl-c", "ctrl-c"])
        written = rec.run()
        self.assertEqual(len(written), 1)
        self.assertEqual(sorted(os.listdir(self.dir)), [written[0]["file"], qr.MANIFEST])

    def test_overflows_reported_by_the_stream_reach_the_manifest(self):
        pre = round(3.2 * qr.SAMPLE_RATE)
        # Block indices: the fake stream's blocks from 0; pick two well after the boundary.
        rec, stream = self.recorder([pre + 2 * qr.FRAMES_PER_SLOT], slots=2, overflow_at={200, 201})
        written = rec.run()
        self.assertEqual(sum(r["overflows"] for r in written), 2)


class HelpersTest(unittest.TestCase):
    def test_q9_mode(self):
        self.assertEqual(qr.q9_mode("Q9 1;"), "iq")
        self.assertEqual(qr.q9_mode("Q91;"), "iq")
        self.assertEqual(qr.q9_mode("Q9 0;"), "audio")
        self.assertIsNone(qr.q9_mode("?;"))
        self.assertIsNone(qr.q9_mode(""))
        self.assertIsNone(qr.q9_mode(None))

    def test_next_boundary_and_name(self):
        self.assertEqual(qr.next_boundary(T0), T0)
        self.assertEqual(qr.next_boundary(T0 + 0.001), T0 + 15)
        self.assertEqual(qr.slot_name(T0 + 45), "261102_120045.wav")


if __name__ == "__main__":
    unittest.main()
