#!/usr/bin/env python3
"""The QSO store's journal, written a second time from its header's description.

Prints the bytes test_qso_store.cpp holds as kGolden: the format in
components/qso_store/include/qso_store.h, each kind of entry once, with zlib's
CRC-32. The test fails unless the store writes the same bytes, so the format
and the CRC are each checked against an implementation that shares no code with
them. Run it again, and paste its output over kGolden, only when the format is
changed on purpose: python3 host_mock/qso_store_golden.py
"""
import struct, zlib
out = bytearray()
h = b"MFQS" + struct.pack("<HHQI", 1, 24, 0x0123456789ABCDEF, 0)
out += h + struct.pack("<I", zlib.crc32(h))
def entry(kind, flags, rid, prev, fields):
    global out
    at = len(out)
    head = struct.pack("<BBHI", 0xD5, kind, len(fields), rid)
    tail = struct.pack("<BBBB", flags, 0, 0, 0)
    crc = zlib.crc32(head + tail + fields)
    out += head + struct.pack("<I", prev) + tail + fields + struct.pack("<I", crc)
    return at
a = entry(1, 0, 1, 0, b"<call:5>K1ABC<band:3>20M<mode:3>FT8<qso_date:8>20261119<time_on:6>215400")
entry(2, 0, 1, a, b"<app_qrzlog_logid:9>130877825")
entry(4, 0, 0, 0, b"qrz_modsince=2026-11-19")
b = entry(1, 1, 2, 0, b"<call:4>W9XY<app_qrzlog_logid:1>7")
entry(3, 0, 2, b, b"")
print(len(out))
for i in range(0, len(out), 16):
    print("        " + " ".join("0x%02X," % x for x in out[i:i+16]))
