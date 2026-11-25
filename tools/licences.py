#!/usr/bin/env python3
"""The licences the station's code ships under, for Settings › Help & about › Licences
(docs/architecture/webapp.md, Settings › Help & about).

Both firmware images link code we did not write, under licences that ask for their notice to travel
with it. ITEMS below says, for each project, which libraries in a linker map are its; the linker maps
say what each image actually holds. So a library that comes or goes cannot be missed:

  python3 tools/licences.py              # writes webapp/app/licences.json from both builds
  python3 tools/licences.py --check MAP… # every library MAP links is credited, as licences.json says

Writing needs both firmware builds (poe_p4/build and sidekick/build, each with its bootloader) and
ESP-IDF's toolchain on PATH, since the texts come from the files each project ships. --check needs only
the maps and licences.json, so CI runs it after each firmware build (tools/check.sh p4, sidekick).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "webapp" / "app" / "licences.json"
IDF = os.environ.get("IDF_PATH", "")

# The images, by the name the page shows, and the maps that make each one up.
IMAGES = {
    "Main board": ["poe_p4/build/poe_p4.map", "poe_p4/build/bootloader/bootloader.map"],
    "Sidekick": ["sidekick/build/sidekick.map", "sidekick/build/bootloader/bootloader.map"],
}

# A text is (root, path) or (root, path, first line, last line), the lines regular expressions
# that bound the notice in a longer file. Roots: repo, idf, gcc (the toolchain's own licences).
APACHE = ("idf", "LICENSE")
# Each project: what it is called, whose it is, its licence, where it lives, its notice, and what
# it claims in a map. "archives" are library names (libfoo.a is foo); "repo" claims every library
# built from a component in this repository that nothing else claims; "idf" every library built
# from ESP-IDF's tree. Order is the page's order.
ITEMS = [
    # Our own components grew from Mini-FT8's code, so its notice is the one they carry: its own file,
    # copied verbatim from github.com/wcheng95/Mini-FT8 (main, LICENSE).
    dict(name="Mini-FT8", by="Wei Cheng AG6AQ, with Zhenxing N6HAN", licence="MIT",
         url="https://github.com/wcheng95/Mini-FT8", text=("repo", "tools/Mini-FT8-LICENSE.txt"), repo=True),
    dict(name="ft8_lib", by="Kārlis Goba", licence="MIT", url="https://github.com/kgoba/ft8_lib",
         text=("repo", "components/ft8_lib/vendor/LICENSE"), archives=["ft8_lib"]),
    dict(name="ESP-IDF", by="Espressif Systems", licence="Apache-2.0", url="https://github.com/espressif/esp-idf",
         text=APACHE, idf=True),
    dict(name="esp-usb", by="Espressif Systems", licence="Apache-2.0", url="https://github.com/espressif/esp-usb",
         text=("repo", "components/usb/LICENSE"),
         archives=["usb", "espressif__usb_host_cdc_acm", "espressif__usb_host_uac"]),
    dict(name="mDNS", by="Espressif Systems", licence="Apache-2.0", url="https://github.com/espressif/esp-protocols",
         text=APACHE, archives=["espressif__mdns"]),
    dict(name="FreeRTOS Kernel", by="Amazon.com, Inc.", licence="MIT", url="https://github.com/FreeRTOS/FreeRTOS-Kernel",
         text=("idf", "components/freertos/FreeRTOS-Kernel/LICENSE.md"), archives=["freertos"]),
    dict(name="lwIP", by="Swedish Institute of Computer Science", licence="BSD-3-Clause", url="https://savannah.nongnu.org/projects/lwip/",
         text=("idf", "components/lwip/lwip/COPYING"), archives=["lwip"]),
    dict(name="Mbed TLS", by="The Mbed TLS Contributors", licence="Apache-2.0", url="https://github.com/Mbed-TLS/mbedtls",
         text=APACHE, archives=["mbedcrypto", "mbedtls", "mbedx509"]),
    dict(name="wpa_supplicant", by="Jouni Malinen and contributors", licence="BSD-3-Clause", url="https://w1.fi/wpa_supplicant/",
         text=("idf", "components/wpa_supplicant/README", r"^Copyright \(c\)", r"OF THE POSSIBILITY OF SUCH DAMAGE\.$"),
         archives=["wpa_supplicant"]),
    dict(name="http-parser", by="Joyent, Inc. and other Node contributors", licence="MIT", url="https://github.com/nodejs/http-parser",
         text=("idf", "components/http_parser/LICENSE.txt"), archives=["http_parser"]),
    dict(name="esp_littlefs", by="Brian Pugh", licence="MIT", url="https://github.com/joltwallet/esp_littlefs",
         text=("repo", "poe_p4/managed_components/joltwallet__littlefs/LICENSE"), archives=["joltwallet__littlefs"]),
    dict(name="littlefs", by="Arm Limited", licence="BSD-3-Clause", url="https://github.com/littlefs-project/littlefs",
         text=("repo", "poe_p4/managed_components/joltwallet__littlefs/src/littlefs/LICENSE.md"), archives=["joltwallet__littlefs"]),
    dict(name="Xtensa HAL", by="Cadence Design Systems, Inc.", licence="MIT", url="https://github.com/espressif/esp-idf/tree/master/components/xtensa",
         text=("idf", "components/xtensa/include/xtensa/hal.h", r"Copyright \(c\)", r"SOFTWARE\.$"), archives=["xt_hal", "xtensa"]),
    dict(name="Newlib", by="Red Hat, Inc. and others", licence="BSD and similar", url="https://sourceware.org/newlib/",
         text=("idf", "components/newlib/COPYING.NEWLIB"), archives=["c", "m"]),
    dict(name="GCC runtime libraries", by="Free Software Foundation, Inc.", licence="GPL-3.0 with the GCC Runtime Library Exception",
         url="https://gcc.gnu.org/", text=("gcc", "COPYING.RUNTIME"), archives=["gcc", "stdc++"]),
    # The app's own: in the sidekick's image as its pack, and in no map.
    dict(name="Inter", by="The Inter Project Authors", licence="OFL-1.1", url="https://github.com/rsms/inter",
         text=("repo", "webapp/tools/Inter-LICENSE.txt"), app=True),
]


def die(msg):
    print("licences: " + msg, file=sys.stderr)
    sys.exit(1)


def linked(map_path):
    """The libraries a linker map put into its image: {name: path as the map gives it}. Read from
    the memory map, after the discarded sections, so an archive whose every section was dropped does
    not count."""
    text = Path(map_path).read_text(errors="replace")
    start = text.find("\nLinker script and memory map")
    if start < 0:
        die(f"{map_path}: no memory map in it")
    out = {}
    for path in re.findall(r"([^\s()]+\.a)\(", text[start:]):
        m = re.search(r"lib([^/\\]+)\.a$", path)
        if m:
            out.setdefault(m.group(1), path)
    return out


def components(map_path):
    """{component: its directory} from the project description beside a map, so a library built
    from a component can be told ours, ESP-IDF's or managed. ({}, '') when there is none."""
    desc = Path(map_path).parent / "project_description.json"
    if not desc.exists():
        return {}, ""
    d = json.loads(desc.read_text())
    return {k: v.get("dir", "") for k, v in d.get("build_component_info", {}).items()}, d.get("idf_path", "")


def claims(lib, path, comps, idf_path):
    """The items that claim one linked library, by name."""
    d = comps.get(lib, "")
    if not d and path.startswith("esp-idf/"):
        d = comps.get(path.split("/")[1], "")
    under_idf = bool(idf_path) and (d or path).startswith(idf_path.rstrip("/") + "/")
    named = [it["name"] for it in ITEMS if lib in it.get("archives", [])]
    out = list(named)
    if under_idf:
        out += [it["name"] for it in ITEMS if it.get("idf")]
    elif d and not named and "/managed_components/" not in d and not under_idf:
        out += [it["name"] for it in ITEMS if it.get("repo")]
    return out


def where(maps_by_image):
    """{item name: [images]} and the libraries no item claims, from the maps of each image."""
    seen, orphans = {}, []
    for image, maps in maps_by_image.items():
        for mp in maps:
            comps, idf_path = components(mp)
            for lib, path in linked(mp).items():
                names = claims(lib, path, comps, idf_path)
                if not names:
                    orphans.append(f"{image}: lib{lib}.a ({path})")
                for n in names:
                    seen.setdefault(n, [])
                    if image not in seen[n]:
                        seen[n].append(image)
    for it in ITEMS:
        if it.get("app"):
            seen[it["name"]] = ["App"]
    return seen, sorted(set(orphans))


def read_text(spec):
    root, rel = spec[0], spec[1]
    if root == "repo":
        path = ROOT / rel
    elif root == "idf":
        if not IDF:
            die("IDF_PATH is not set: source esp-idf/export.sh")
        path = Path(IDF) / rel
    else:
        gcc = shutil.which("riscv32-esp-elf-gcc") or die("riscv32-esp-elf-gcc not on PATH: source esp-idf/export.sh")
        path = Path(gcc).resolve().parent.parent / "share" / "licenses" / "gcc" / rel
    if not path.exists():
        die(f"{path}: not found (build both images first, so their managed components are fetched)")
    text = path.read_text(errors="replace")
    if len(spec) > 2:
        lines = text.splitlines()
        a = next((i for i, l in enumerate(lines) if re.search(spec[2], l.strip(" */"))), None)
        b = next((i for i, l in enumerate(lines) if a is not None and i >= a and re.search(spec[3], l.strip(" */"))), None)
        if a is None or b is None:
            die(f"{path}: the notice's bounds were not found")
        text = "\n".join(re.sub(r"^\s*\*? ?", "", l) if spec[0] != "repo" and rel.endswith(".h") else l
                         for l in lines[a:b + 1])
    return text.strip() + "\n"


def model(seen):
    """The page's list: each item as it shows, linked to a text carried once. Only the items some
    image holds."""
    texts, items = {}, []
    for it in ITEMS:
        if it["name"] not in seen:
            continue
        body = read_text(it["text"])
        key = hashlib.sha256(re.sub(r"\s+", " ", body).encode()).hexdigest()[:12]
        texts.setdefault(key, body)
        items.append(dict(name=it["name"], by=it["by"], licence=it["licence"], url=it["url"],
                          where=seen[it["name"]], text=key))
    return {"items": items, "texts": texts}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", nargs="+", metavar="MAP",
                    help="check that every library these maps (one image's) link is credited in licences.json")
    a = ap.parse_args()
    if a.check:
        seen, orphans = where({"this image": a.check})
        if orphans:
            die("no item in tools/licences.py claims:\n  " + "\n  ".join(orphans))
        have = {i["name"] for i in json.loads(OUT.read_text())["items"]}
        missing = sorted(set(seen) - have)
        if missing:
            die("linked but not in webapp/app/licences.json: " + ", ".join(missing) + " (run python3 tools/licences.py)")
        print(f"licences: every library {len(a.check)} map(s) link is credited")
        return
    maps = {img: [str(ROOT / m) for m in ms] for img, ms in IMAGES.items()}
    for ms in maps.values():
        for m in ms:
            if not Path(m).exists():
                die(f"{m}: build both images first (idf.py -C poe_p4 build; idf.py -C sidekick build)")
    seen, orphans = where(maps)
    if orphans:
        die("no item in ITEMS claims:\n  " + "\n  ".join(orphans))
    data = model(seen)
    OUT.write_text(json.dumps(data, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"licences: {len(data['items'])} projects, {len(data['texts'])} texts -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
