#!/usr/bin/env python3
"""Record the main board's console from its PROG port, one JSON object a line, for the decode bench.

The main board prints each slot's decode statistics to its console and not to Port A, so they take no places
in the sidekick's event ring (docs/architecture/decode.md, **Slot lines**): `SLOT` and `BDRY` for every slot,
`SKIP`, `BUSY` or `PAUSED` for one not decoded, each with `/FT4` for FT4's channel beside FT8. This records
them, and every other console line, with the time this computer read it:

  {"t": 1795094527.31, "x": "SLOT 119672968 c256 m20 t0 dec1349 left351 y0 u5308p2 b79/77"}

`x` is the line's message, without the log level, uptime and tag the console puts before it.
tools/decode_bench/score_events.py reads the file beside a recorded `/api/events` log (`--console`).

  python3 tools/p4_console.py PORT OUT.jsonl [--minutes N]

PORT is the main board's PROG port (an Espressif USB serial device: COM5, /dev/ttyACM0 or
/dev/cu.usbmodem1234; the sidekick is one too, so name the port rather than guess). OUT is appended to, so a
run stopped and started again keeps one file. DTR and RTS stay asserted, as the port opens with them: on the
board's USB-Serial-JTAG, DTR low with RTS high resets it, so a running station keeps running. This computer's
clock must be synced (NTP, or your OS's time sync): `t` is its time. Needs `pyserial` (python3 -m pip install pyserial).
"""
import argparse
import json
import re
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# ESP-IDF's log prefix: level, uptime in ms, tag ("I (7686) adv: ...").
PREFIX = re.compile(r"^[EWIDV] \(\d+\) [^:]*: ")


def message(raw):
    """The message of one console line: its colour codes, log prefix and line end gone. None if empty."""
    text = ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\r\n")
    text = PREFIX.sub("", text, count=1).rstrip()
    return text or None


def record(lines, out, now=time.time):
    """Writes one row per line of `lines` (bytes, each ending in a newline) to `out`. Returns the rows written."""
    n = 0
    for raw in lines:
        x = message(raw)
        if x is None:
            continue
        out.write(json.dumps({"t": round(now(), 2), "x": x}) + "\n")
        out.flush()
        n += 1
    return n


def port_lines(port, until):
    import serial  # only here, so the host test needs no pyserial

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 1
    # Left as pyserial opens them, both asserted: DTR low with RTS high is the USB-Serial-JTAG's reset, and
    # setting them before open() passes through that state (an ESP32-S3 reset on 2026-11-19).
    s.open()
    buf = b""
    while time.time() < until:
        buf += s.read(4096)
        *done, buf = buf.split(b"\n")
        for line in done:
            yield line + b"\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port")
    ap.add_argument("out")
    ap.add_argument("--minutes", type=float, default=float("inf"), help="stop after this long (default: never)")
    args = ap.parse_args()
    until = time.time() + args.minutes * 60
    with open(args.out, "a") as out:
        try:
            n = record(port_lines(args.port, until), out)
        except KeyboardInterrupt:
            n = None
    print(f"{args.out}: {'stopped' if n is None else str(n) + ' lines'}", file=sys.stderr)


if __name__ == "__main__":
    main()
