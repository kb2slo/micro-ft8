#!/usr/bin/env python3
"""run_on_device.py's 'auto' port: the first Espressif USB serial port by vendor ID, whatever the OS calls it
(test_apps/decode_bench/README.md). Run by tools/check.sh (tools)."""
import os
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_on_device  # noqa: E402


def port(device, vid):
    return types.SimpleNamespace(device=device, vid=vid)


class Resolve(unittest.TestCase):
    def ports(self, *found):
        return mock.patch.object(run_on_device.serial.tools.list_ports, "comports", return_value=list(found))

    def test_a_named_port_is_used_as_given(self):
        with self.ports():
            self.assertEqual(run_on_device.resolve("/dev/ttyACM3"), "/dev/ttyACM3")
            self.assertEqual(run_on_device.resolve("COM7"), "COM7")

    def test_auto_finds_the_espressif_port_by_vendor_id_on_any_os(self):
        for name in ("/dev/ttyACM0", "/dev/cu.usbmodem1101", "COM5"):
            with self.ports(port("/dev/ttyS0", None), port(name, 0x303A)):
                self.assertEqual(run_on_device.resolve("auto"), name)

    def test_auto_skips_another_usb_serial_device(self):
        # A QMX on the same computer (STMicroelectronics, 0x0483) is not the P4.
        with self.ports(port("/dev/ttyACM0", 0x0483), port("/dev/ttyACM1", 0x303A)):
            self.assertEqual(run_on_device.resolve("auto"), "/dev/ttyACM1")

    def test_auto_with_none_found_is_empty(self):
        with self.ports(port("/dev/ttyACM0", 0x0483)):
            self.assertEqual(run_on_device.resolve("auto"), "")


if __name__ == "__main__":
    unittest.main()
