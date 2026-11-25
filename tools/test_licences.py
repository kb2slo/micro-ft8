#!/usr/bin/env python3
"""tools/licences.py on a synthetic build: which item claims each library a linker map links, that a
library dropped by the linker is not counted, and that one nothing claims fails the check CI runs
after each firmware build."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import licences  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
failed = 0


def check(name, fn):
    global failed
    try:
        fn()
        print("PASS  " + name)
    except AssertionError as e:
        failed += 1
        print(f"FAIL  {name}\n      {e}")


def build(tmp, libs, discarded=()):
    """A build directory with a map linking libs ({lib: archive path}) and a project description
    naming each component's directory."""
    idf, repo = "/opt/idf", "/work/micro-ft8"
    dirs = {"freertos": idf + "/components/freertos", "adif": repo + "/components/adif",
            "usb": repo + "/components/usb", "espressif__mdns": repo + "/sidekick/managed_components/espressif__mdns"}
    (tmp / "project_description.json").write_text(json.dumps(
        {"idf_path": idf, "build_component_info": {k: {"dir": v} for k, v in dirs.items()}}))
    lines = ["Archive member included to satisfy reference by file (symbol)", "",
             "Discarded input sections", ""]
    lines += [f" .text.x  0x0  0x4 {p}(x.o)" for p in discarded]
    lines += ["", "Memory Configuration", "", "Linker script and memory map", ""]
    lines += [f" .text    0x40000000  0x10 {p}(a.c.obj)" for p in libs.values()]
    (tmp / "app.map").write_text("\n".join(lines) + "\n")
    return tmp / "app.map"


def _eq(a, b):
    assert a == b, f"{a!r} != {b!r}"


def claimed(mp):
    comps, idf = licences.components(mp)
    return {lib: licences.claims(lib, path, comps, idf) for lib, path in licences.linked(mp).items()}


with tempfile.TemporaryDirectory() as t:
    tmp = Path(t)
    mp = build(tmp, {
        "freertos": "esp-idf/freertos/libfreertos.a", "adif": "esp-idf/adif/libadif.a",
        "usb": "esp-idf/usb/libusb.a", "espressif__mdns": "esp-idf/espressif__mdns/libespressif__mdns.a",
        "net80211": "/opt/idf/components/esp_wifi/lib/esp32s3/libnet80211.a",
        "c": "/opt/tools/xtensa-esp-elf/lib/libc.a",
    }, discarded=["/opt/tools/lib/libunused.a"])
    got = claimed(mp)

    check("an ESP-IDF component is ESP-IDF's, and FreeRTOS's when it carries the kernel",
          lambda: _eq(sorted(got["freertos"]), ["ESP-IDF", "FreeRTOS Kernel"]))
    check("a binary library under ESP-IDF's tree is ESP-IDF's", lambda: _eq(got["net80211"], ["ESP-IDF"]))
    check("our own component carries Mini-FT8's notice, the code it grew from", lambda: _eq(got["adif"], ["Mini-FT8"]))
    check("a vendored component in our tree is its project's, not ours", lambda: _eq(got["usb"], ["esp-usb"]))
    check("a managed component is its project's", lambda: _eq(got["espressif__mdns"], ["mDNS"]))
    check("a toolchain library is claimed by name", lambda: _eq(got["c"], ["Newlib"]))
    check("a library the linker discarded is not counted", lambda: _eq("unused" in got, False))

    orphan = build(Path(tempfile.mkdtemp(dir=t)), {"mystery": "/opt/tools/lib/libmystery.a"})

    def fails_unclaimed():
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "licences.py"), "--check", str(orphan)],
                           capture_output=True, text=True)
        assert r.returncode != 0 and "libmystery.a" in r.stderr, (r.returncode, r.stderr)
    check("--check fails on a library nothing claims, and names it", fails_unclaimed)


if failed:
    print(f"{failed} failed")
    sys.exit(1)
print("all passed")
