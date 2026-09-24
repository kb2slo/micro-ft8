#!/usr/bin/env python3
"""Flash a decode_bench app image to the app slot only (0x10000) and capture its output.

Leaves the bootloader, partition table, nvs and storage partitions untouched, so it is safe on a board
that also runs the firmware (same partition table; see sdkconfig.defaults.<target>).

  python tools/run_on_device.py PORT build_dir/decode_bench.bin LOGFILE [--chip esp32p4]
Run inside the ESP-IDF environment (needs esptool and pyserial).
"""
import argparse
import subprocess
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("image")
    ap.add_argument("log")
    ap.add_argument("--chip", default="esp32p4")
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()

    subprocess.run([sys.executable, "-m", "esptool", "--chip", args.chip, "--port", args.port, "--after", "no_reset",
                    "write_flash", "0x10000", args.image], check=True)
    time.sleep(1.0)
    with serial.Serial(args.port, 115200, timeout=1) as port, open(args.log, "w") as log:
        port.setDTR(False)          # hard reset into the app (USB-Serial-JTAG: RTS pulse)
        port.setRTS(True)
        time.sleep(0.1)
        port.setRTS(False)
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            line = port.readline().decode("utf-8", "replace")
            if not line:
                continue
            log.write(line)
            log.flush()
            if line.startswith(("RESULT", "BENCH")):
                print(line.rstrip(), flush=True)
            if line.startswith("BENCH done"):
                return 0
    print("timed out waiting for BENCH done", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
