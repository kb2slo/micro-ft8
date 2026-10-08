# sidekick

M5Stack AtomS3 Lite (ESP32-S3FN8) firmware for the Micro-FT8 companion link.

Why this board, and its hardware facts: [`sidekick.md`](../docs/architecture/sidekick.md) (**Hardware**). A sidekick never needs to be a USB host, so the AtomS3 Lite's device-only USB-C ([`system.md`](../docs/architecture/system.md), **Why not**) does not matter here.
How it works and why: [`docs/architecture/sidekick.md`](../docs/architecture/sidekick.md). Planned work: [`docs/ROADMAP.md`](../docs/ROADMAP.md). Do not duplicate the plan here.

Separate `idf.py` project from `poe_p4`: ESP-IDF locks target *and sdkconfig* per project.

## Build (desk)

```bash
cd sidekick
idf.py build
idf.py flash monitor
```

Target is pinned to `esp32s3` in `CMakeLists.txt`; no `set-target` step needed. The partition table is `partitions.csv`: two 3 MB OTA slots, no factory ([`sidekick.md`](../docs/architecture/sidekick.md) (**Firmware updates**)). With `otadata` erased the bootloader boots `ota_0`. Flash size is pinned to 8 MB in `sdkconfig.defaults` — without it the build takes ESP-IDF's 2 MB default and writes a wrong flash-size field into the bootloader header. `sdkconfig` is gitignored and regenerated, so delete it after changing the defaults or the old value sticks. `PROJECT_VER` is also pinned there — exact git SHA + dirty flag, not ESP-IDF's default `git describe` — so a build's identity is exact, not a fuzzy tag-relative string.

## First-time setup (a newly flashed sidekick)

The firmware carries only the setup pages. The app (`app.html`) is downloaded once, through your phone, which needs the internet for that one step. The sidekick's own AP has none, so either the phone uses its cellular data while joined to the AP (quickest; an iPhone does this by itself), or the sidekick joins your home WiFi (2.4 GHz only). Why: [`sidekick.md`](../docs/architecture/sidekick.md) (**Installing the app bundle**).

1. Flash the sidekick ([Build](#build-desk), or the merged image at `0x0`). Join its WiFi network and open `http://192.168.4.1/`. Until the app is installed the page shows the first-time steps instead of "Operate in AP mode".
2. **Over cellular:** stay on the sidekick's network and open `http://192.168.4.1/update`. Press the sidekick's button when asked, then tap **Install**. It opens the app when done.
   **Or over home WiFi:** press the sidekick's button, pick your home WiFi, enter its password, **Save** (an iPhone Personal Hotspot needs *Maximize Compatibility* on or it will not be listed). Join the same WiFi on your phone and open `http://micro-ft8.local/update`, press the button when asked, then tap **Install**.

**To start over** (testing, or handing the unit on): Settings > Network > **Factory reset**, then confirm. It erases the saved WiFi, the installed app, the pairing, the bundle host override and the AP-mode flag, and the sidekick restarts as a new unit. It also resets the main board's station settings to defaults (a second prompt offers to delete its QSO logs too), and refuses if the main board is not linked or is transmitting. If you cannot reach the app, `idf.py erase-flash` then flash again does the same and more. 

**Handing a unit on** to another operator: factory reset and delete the logs too, then install the app over cellular (step 2 above) and join no WiFi network. It then arrives on its own network with the app ready, which is what the getting-started sheet in the box assumes: Settings shows Mode **AP (offline)** and Call `YOURCALL`. Print the sheet from a fresh build, so it describes the current screens: `npm ci --prefix webapp/tools` once, then `node webapp/tools/getting_started.mjs --pdf` writes `webapp/dist/getting-started.pdf`.

After that, the app is on the sidekick: Settings > Network > **Switch to AP mode** runs it with no network at all. Opening `/app.html` before step 4 shows a "not installed yet" page that points back here. If Install says it cannot reach the host, the phone has no internet: check it is on your home WiFi.

## Backing up a stock unit

**Before first flashing a specific physical unit, back it up** — this lets you restore it afterward instead of the part being a one-shot:

```bash
esptool.py --chip esp32s3 -b 460800 \
    read_flash 0x0 0x800000 stock_backup/atoms3_stock_backup.bin
```

Note both changes from the NanoC6 form: `--chip esp32s3`, and `0x800000` because the AtomS3 Lite has 8 MB
where the Nano had 4 MB. Reading only the first 4 MB would produce a backup that silently restores garbage
over the upper half.

`../tools/restore_sidekick_stock.sh` writes it back afterward (port optional; it autodetects). `stock_backup/` is gitignored on purpose (vendor firmware, and a snapshot of one specific unit) — this is a personal safety net you (re)create per device, not a repo asset.

## Version identity

Every ESP-IDF app embeds an `esp_app_desc_t` (`project_name`, `version`) as the literal first bytes of its DROM segment — for this build, absolute flash offset **`0x20020`** (was `0x10020` before the OTA partition change moved the app from `factory` at `0x10000` to `ota_0` at `0x20000`). Re-derive if target, IDF version, partition table or secure boot changes — **re-verified after the esp32s3 retarget on 2026-09-08 and unchanged**: S3 and C6 both put the bootloader at `0x0` and the app at `0x10000`, and the descriptor is 0x20 into the image. Checked by reading the built `sidekick.bin`, not assumed.

## Status

Serves the web app, provisioning and pairing, and relays between the browser and the main board over Port A. How it works and why: [`docs/architecture/sidekick.md`](../docs/architecture/sidekick.md).

- **Grove pins are GPIO1/GPIO2** (silkscreen `G`, `5V`, `G2`, `G1`; M5Stack labels Grove pins by GPIO number). With the straight-through cable the main board receives on G1 and transmits on G2, so the sidekick transmits on G1 and receives on G2. Confirmed on hardware 2026-09-09. Neither is an S3 strapping pin (GPIO0/3/45/46).
- **Port A can never flash a stock chip (B42).** The ROM bootloader listens only on UART0's default pins (GPIO43/44 on S3), and Grove is GPIO1/GPIO2. USB-C is the only wire flash path; updates come over WiFi ([RFC 0007](../docs/rfcs/0007-flash-and-update.md)).
