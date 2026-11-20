#!/usr/bin/env python3
"""Host test for tools/p4_console.py, and for score_events.py reading what it records: no board needed.

  python3 tools/test_p4_console.py
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "decode_bench"))
import p4_console as pc  # noqa: E402
import score_events as se  # noqa: E402

# As the main board prints them: colour codes, level, uptime and tag before the message.
SLOT = b"\x1b[0;32mI (7686) main: SLOT 119672901 c256 m15 t0 dec1562 left99 y0 u6473p2 b83/78\x1b[0m\r\n"
FT4 = b"I (6016) main: SLOT/FT4 239345803 c256 m0 t0 dec618 left1806 y0 u2782p1! b93/78\r\n"
SKIP = b"I (9100) main: SKIP 119672902\r\n"
LATE = b"I (9300) main: SLOT 119672903 c256 m9 t0 dec1700 left-40 y0 u6473p2 b83/78\r\n"


class Message(unittest.TestCase):
    def test_prefix_and_colour_go(self):
        self.assertEqual(pc.message(SLOT), "SLOT 119672901 c256 m15 t0 dec1562 left99 y0 u6473p2 b83/78")
        self.assertEqual(pc.message(SKIP), "SKIP 119672902")

    def test_a_line_with_no_prefix_is_kept_whole(self):
        self.assertEqual(pc.message(b"ESP-ROM:esp32p4-eco2-20240710\r\n"), "ESP-ROM:esp32p4-eco2-20240710")

    def test_blank_and_bad_bytes(self):
        self.assertIsNone(pc.message(b"\r\n"))
        self.assertEqual(pc.message(b"I (1) x: caf\xe9\n"), "caf�")


class Record(unittest.TestCase):
    def test_one_row_a_line_with_the_time_read(self):
        out = io.StringIO()
        clock = iter([100.004, 101.5, 102.0])
        n = pc.record([SLOT, b"\r\n", SKIP, FT4], out, now=lambda: next(clock))
        rows = [json.loads(line) for line in out.getvalue().splitlines()]
        self.assertEqual(n, 3)
        self.assertEqual([r["t"] for r in rows], [100.0, 101.5, 102.0])
        self.assertEqual(rows[1], {"t": 101.5, "x": "SKIP 119672902"})


class ScoreWithConsole(unittest.TestCase):
    """score_events.py reads the console's SLOT and SKIP rows as it read them from the events."""

    def setUp(self):
        t0 = 119672901 * 15
        self.events = [
            {"s": 1, "t": t0 + 14, "d": 1, "x": "CQ K1ABC FN42", "snr": -5, "hz": 1500},
            {"s": 2, "t": t0 + 16, "d": 4, "active": 1, "x": "K1ABC KB2SLO FN30", "hz": 1200},
            {"s": 3, "t": t0 + 29, "d": 4, "active": 0, "x": "K1ABC KB2SLO FN30", "hz": 1200},
        ]
        clock = iter([t0 + 14.6, t0 + 15.2, t0 + 28.0, t0 + 44.6])
        out = io.StringIO()
        pc.record([SLOT, FT4, SKIP, LATE], out, now=lambda: next(clock))
        self.dir = tempfile.TemporaryDirectory()
        self.console = os.path.join(self.dir.name, "p4.jsonl")
        with open(self.console, "w") as f:
            f.write(out.getvalue())

    def tearDown(self):
        self.dir.cleanup()

    def test_merged_by_time(self):
        merged = se.merge(self.events, se.load_console(self.console))
        self.assertEqual([e["d"] for e in merged], [1, 0, 0, 4, 0, 4, 0])
        self.assertEqual(merged[1]["x"].split()[0], "SLOT")

    def test_decode_load_and_timeline_read_the_console(self):
        merged = se.merge(self.events, se.load_console(self.console))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            se.decode_load(merged, se.tx_starts(merged))
            se.timeline(merged, "K1ABC")
        text = buf.getvalue()
        self.assertIn("FT8: 2 decoded", text)
        self.assertIn("FT4: 1 decoded", text)
        self.assertIn("FT8 left -40 ms", text)
        self.assertIn("FT8 skip 119672902", text)


if __name__ == "__main__":
    unittest.main()
