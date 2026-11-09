#!/usr/bin/env python3
"""Reads a QMX's configuration menus over CAT with the Menu Manager (MM, ML), and nothing else.

    menu_probe.py --port <QMX serial port>
    menu_probe.py --port <QMX serial port> --walk "System config"

Prints, for the radio on this computer's USB:
  * VN; OM; and the radio's state (IF;) before and after, so a read that moved the radio shows.
  * The "Band config." table: every row the radio describes (MMBand config.|<i>?;), read in
    every column (MM<row>[<col>];), with each list row's allowed values (ML<n>;).
  * The menu under each --walk path (default "System config"), item by item, with its value.

It only reads: it sends no MM Set (`=`), no MU;, no TX;. The paths and replies it prints are what
the main board's link-up read relies on (docs/architecture/radio.md, Menu Manager).
"""

import argparse
import re
import sys
import time

import serial

ESC_SEQ = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b.")


class Cat:
    def __init__(self, port, timeout_s):
        self.port = serial.Serial(port, 115200, timeout=0.05)
        self.timeout_s = timeout_s
        self.buf = ""

    def ask(self, cmd):
        """Sends one command and returns its reply without the trailing ';', or None on no reply."""
        self.port.reset_input_buffer()
        self.buf = ""
        self.port.write(cmd.encode())
        end = time.monotonic() + self.timeout_s
        while time.monotonic() < end:
            self.buf += self.port.read(256).decode(errors="replace")
            text = ESC_SEQ.sub("", self.buf)
            if ";" in text:
                return text.split(";", 1)[0].strip()
        return None


def describe(cat, path):
    """MM<path>?; -> (type, param, name) or None. Name keeps any [n] column count."""
    r = cat.ask(f"MM{path}?;")
    if r is None or not r.startswith("MM"):
        return None
    parts = r[2:].split("|", 2)
    if len(parts) != 3:
        return None
    try:
        return int(parts[0]), int(parts[1]), parts[2]
    except ValueError:
        return None


TYPES = {0: "sub-menu", 1: "action", 2: "string", 3: "number", 4: "byte", 5: "list", 6: "info", 7: "mask"}


def list_values(cat, list_type, cache):
    if list_type not in cache:
        r = cat.ask(f"ML{list_type};")
        cache[list_type] = r[2:] if r and r.startswith("ML") else f"<{r}>"
    return cache[list_type]


def value(cat, path):
    r = cat.ask(f"MM{path};")
    if r is None:
        return "<no reply>"
    return r[2:] if r.startswith("MM") else f"<{r}>"


def band_table(cat, lists):
    top = None
    for i in range(32):
        d = describe(cat, str(i))
        if d is None:
            break
        if d[2].lower().startswith("band config"):
            top = (i, d)
            break
    if top is None:
        print("Band config.: not found on the top-level menu")
        return
    m = re.search(r"\[(\d+)\]", top[1][2])
    cols = int(m.group(1)) if m else 16
    print(f"\nBand config. (top-level index {top[0]}, reply name {top[1][2]!r}, {cols} columns)")
    for row in range(64):
        d = describe(cat, f"Band config.|{row}")
        if d is None:
            break
        typ, param, name = d
        cells = [value(cat, f"Band config.|{name}[{c}]") for c in range(cols)]
        extra = f"  values: {list_values(cat, param, lists)}" if typ in (5, 7) else ""
        print(f"  [{row}] {name!r} type {TYPES.get(typ, typ)} param {param}{extra}")
        print("       " + " | ".join(cells))


def walk(cat, path, lists, depth=0):
    for i in range(64):
        child = f"{path}|{i}"
        d = describe(cat, child)
        if d is None:
            break
        typ, param, name = d
        named = f"{path}|{name}"
        pad = "  " * (depth + 1)
        if typ == 0:
            print(f"{pad}{name}/")
            walk(cat, named, lists, depth + 1)
            continue
        val = value(cat, named) if typ not in (1, 6) else ""
        # A numeric name can be reached only by its index (CAT manual, Menu manager).
        if val.startswith("<") and name.strip().isdigit():
            val = value(cat, child)
        extra = f"  values: {list_values(cat, param, lists)}" if typ in (5, 7) else ""
        print(f"{pad}[{i}] {name!r} = {val!r}  ({TYPES.get(typ, typ)}, param {param}){extra}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--walk", action="append", help='menu path to list (repeatable; default "System config")')
    ap.add_argument("--timeout", type=float, default=0.5, help="seconds to wait for each reply")
    a = ap.parse_args()

    cat = Cat(a.port, a.timeout)
    before = cat.ask("IF;")
    print("VN:", cat.ask("VN;"))
    print("OM:", cat.ask("OM;"))
    print("IF before:", before)

    lists = {}
    started = time.monotonic()
    band_table(cat, lists)
    for path in a.walk or ["System config"]:
        print(f"\n{path}/")
        walk(cat, path, lists)

    after = cat.ask("IF;")
    print(f"\nIF after: {after}")
    print(f"read took {time.monotonic() - started:.1f} s")
    if before != after:
        print("THE RADIO'S STATE CHANGED during the read (compare IF before and after)")
        return 1
    print("radio state unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
