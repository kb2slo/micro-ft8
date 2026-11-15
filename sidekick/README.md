# sidekick

M5Stack AtomS3 Lite (ESP32-S3FN8) firmware for the Micro-FT8 companion link.

Why this board, and its hardware facts: [`sidekick.md`](../docs/architecture/sidekick.md) (**Hardware**). A sidekick never needs to be a USB host, so the AtomS3 Lite's device-only USB-C ([`system.md`](../docs/architecture/system.md), **Why not**) does not matter here.
How it works and why: [`docs/architecture/sidekick.md`](../docs/architecture/sidekick.md). Planned work: [`docs/ROADMAP.md`](../docs/ROADMAP.md). Do not duplicate the plan here.

Separate `idf.py` project from `poe_p4`: ESP-IDF locks target *and sdkconfig* per project.

## Build (desk)

```bash
npm ci --prefix webapp/tools   # once per clone: the web build's packages
cd sidekick
idf.py build
idf.py flash monitor
```

The image carries the web app: every build runs the web build (`webapp/tools/build.py`, which needs Node) and embeds its pack, so flashing the sidekick is also how its app is put on it ([`sidekick.md`](../docs/architecture/sidekick.md) (**Web root**)). For an edit to the app alone, `python3 webapp/tools/install_app.py` pushes the built app onto a running sidekick, which serves it until its firmware next changes (**The bench push**, same doc); a sidekick on a release refuses it.

**Unplug the Grove cable before flashing over USB.** With it connected and the sidekick the only board on USB, the sidekick's 5 V also feeds the main board and it restarts every few seconds; with both boards on USB its port did not enumerate (kernel error -71). Alone on USB it flashes cleanly (bench, 2026-11-15).

Target is pinned to `esp32s3` in `CMakeLists.txt`; no `set-target` step needed. The partition table is `partitions.csv`: two 3 MB OTA slots, no factory ([`sidekick.md`](../docs/architecture/sidekick.md) (**Firmware updates**)). With `otadata` erased the bootloader boots `ota_0`. Flash size is pinned to 8 MB in `sdkconfig.defaults` — without it the build takes ESP-IDF's 2 MB default and writes a wrong flash-size field into the bootloader header. `sdkconfig` is gitignored and regenerated, so delete it after changing the defaults or the old value sticks. `PROJECT_VER` is also pinned there — exact git SHA + dirty flag, not ESP-IDF's default `git describe` — so a build's identity is exact, not a fuzzy tag-relative string.

## First-time setup (a newly flashed sidekick)

The app is in the firmware image, so a newly flashed sidekick serves it at once, with no internet.

1. Flash the sidekick ([Build](#build-desk), or the merged image at `0x0`). Join its WiFi network and open `http://192.168.4.1/`.
2. Tap **Operate in AP mode** to use it on its own network, or pick your home WiFi, enter its password and **Save** (an iPhone Personal Hotspot needs *Maximize Compatibility* on or it will not be listed), then join the same WiFi on your phone and open `http://micro-ft8.local/`.

**To start over** (testing, or handing the unit on): Settings > Diagnostics & reset > **Factory reset**, then confirm. It erases the saved WiFi, the pairing and the AP-mode flag, and the sidekick restarts as a new unit, its app still there. It also resets the main board's station settings to defaults (a second prompt offers to delete its QSO logs too), and refuses if the main board is not linked or is transmitting. If you cannot reach the app, `idf.py erase-flash` then flash again does the same and more. 

**Handing a unit on** to another operator: factory reset and delete the logs too, and join no WiFi network. It then arrives on its own network with the app ready, which is what the getting-started sheet in the box assumes: Settings shows Mode **AP (offline)** and Call `YOURCALL`. Print the sheet from a fresh build, so it describes the current screens: `npm ci --prefix webapp/tools` once, then `node webapp/tools/getting_started.mjs --pdf` writes `webapp/dist/getting-started.pdf`.

Settings > Network > **Switch to AP mode** runs a sidekick that is on your WiFi with no network at all.

## Backing up a stock unit

**Before first flashing a specific physical unit, back it up** — this lets you restore it afterward instead of the part being a one-shot:

```bash
esptool.py --chip esp32s3 -b 460800 \
    read_flash 0x0 0x800000 stock_backup/atoms3_stock_backup.bin
```

`0x800000` is the AtomS3 Lite's whole 8 MB. Reading less would produce a backup that silently restores
garbage over the rest.

`../tools/restore_sidekick_stock.sh` writes it back afterward (port optional; it autodetects). `stock_backup/` is gitignored on purpose (vendor firmware, and a snapshot of one specific unit) — this is a personal safety net you (re)create per device, not a repo asset.

## Version identity

Every ESP-IDF app embeds an `esp_app_desc_t` (`project_name`, `version`) as the literal first bytes of its DROM segment — for this build, absolute flash offset **`0x20020`** (was `0x10020` before the OTA partition change moved the app from `factory` at `0x10000` to `ota_0` at `0x20000`). Re-derive if target, IDF version, partition table or secure boot changes — **re-verified after the esp32s3 retarget on 2026-09-08 and unchanged**: S3 and C6 both put the bootloader at `0x0` and the app at `0x10000`, and the descriptor is 0x20 into the image. Checked by reading the built `sidekick.bin`, not assumed.

## Status

Serves the web app, provisioning and pairing, and relays between the browser and the main board over Port A. How it works and why: [`docs/architecture/sidekick.md`](../docs/architecture/sidekick.md).

- **Grove pins are GPIO1/GPIO2** (silkscreen `G`, `5V`, `G2`, `G1`; M5Stack labels Grove pins by GPIO number). With the straight-through cable the main board receives on G1 and transmits on G2, so the sidekick transmits on G1 and receives on G2. Confirmed on hardware 2026-09-09. Neither is an S3 strapping pin (GPIO0/3/45/46).
- **Port A can never flash a stock chip (B42).** The ROM bootloader listens only on UART0's default pins (GPIO43/44 on S3), and Grove is GPIO1/GPIO2. USB-C is the only wire flash path; updates come over WiFi (epic [ukk6](../docs/roadmap/ota-updates.md)).
