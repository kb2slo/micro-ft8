#!/usr/bin/env python3
"""Flash a decode_bench build and capture its output over USB-Serial-JTAG.

Default: write the app image to the app slot only (0x10000), leaving the bootloader, partition table, nvs and
storage untouched. That is safe on a board that also runs the firmware, because the bench uses the firmware's
partition table (see sdkconfig.defaults.<target>).

--full: write the whole build (bootloader, partition table, app) from the build directory's flash_args, like
`idf.py flash`. nvs and storage are still not written.

  python tools/run_on_device.py PORT build_dir/decode_bench.bin LOGFILE [--chip esp32s3] [--full]
Run inside the ESP-IDF environment (needs esptool and pyserial).
"""
import argparse
import glob
import os
import subprocess
import sys
import time

import serial


def resolve(path):
    """'auto' means the first /dev/cu.usbmodem*: a replug can change the name (1101 -> 101 seen on the ADV)."""
    if path != "auto":
        return path
    found = sorted(glob.glob("/dev/cu.usbmodem*"))
    return found[0] if found else ""


def open_port(path, deadline):
    """The chip reset can re-enumerate USB; wait for the port to come back."""
    while time.time() < deadline:
        path_now = resolve(path)
        if path_now and os.path.exists(path_now):
            path = path_now
            try:
                return serial.Serial(path, 115200, timeout=1)
            except serial.SerialException:
                pass
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="serial port, or 'auto' for the first /dev/cu.usbmodem*")
    ap.add_argument("image")
    ap.add_argument("log")
    ap.add_argument("--chip", default="esp32p4")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--capture-only", action="store_true",
                    help="do not flash or reset; wait for the port and read (after a reset by hand)")
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()

    esptool = [sys.executable, "-m", "esptool", "--chip", args.chip, "--port", resolve(args.port), "--after", "no_reset"]
    if args.capture_only:
        pass
    elif args.full:
        build_dir = os.path.dirname(os.path.abspath(args.image))
        subprocess.run(esptool + ["write_flash", "@flash_args"], cwd=build_dir, check=True)
    else:
        subprocess.run(esptool + ["write_flash", "0x10000", args.image], check=True)
    time.sleep(1.0)

    deadline = time.time() + args.timeout
    port = open_port(args.port, deadline)
    if port is None:
        print("port did not come back; reset the board by hand", file=sys.stderr)
        return 1
    if not args.capture_only:
        # Hard reset into the app (USB-Serial-JTAG: RTS pulse). On a board strapped into download mode by hand
        # (the ADV's G0) this lands back in download mode; use --capture-only and reset by hand instead.
        port.setDTR(False)
        port.setRTS(True)
        time.sleep(0.1)
        port.setRTS(False)

    with open(args.log, "w") as log:
        while time.time() < deadline:
            try:
                line = port.readline().decode("utf-8", "replace")
            except (serial.SerialException, OSError):
                port.close()
                port = open_port(args.port, deadline)
                if port is None:
                    break
                continue
            if not line:
                continue
            log.write(line)
            log.flush()
            if line.startswith(("RESULT", "BENCH")):
                print(line.rstrip(), flush=True)
            if line.startswith("BENCH done"):
                port.close()
                return 0
    print("timed out waiting for BENCH done", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
