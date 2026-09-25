# sidekick

M5Stack AtomS3 Lite (ESP32-S3FN8) firmware for the Micro-FT8 companion link.

Why this board, and its hardware facts: [`sidekick.md`](../docs/architecture/sidekick.md) (**Hardware**). A sidekick never needs to be a USB host, so the AtomS3 Lite's device-only USB-C ([`system.md`](../docs/architecture/system.md), **Why not**) does not matter here.
How it works and why: [`docs/architecture/sidekick.md`](../docs/architecture/sidekick.md). Planned work: [`docs/ROADMAP.md`](../docs/ROADMAP.md). Do not duplicate the plan here.

Separate `idf.py` project from the ADV app — ESP-IDF locks target *and sdkconfig* per project, so the two cannot share one configure even now that both are `esp32s3`. The ADV build embeds this project's output ([`sidekick.md`](../docs/architecture/sidekick.md), **Installing the sidekick from the ADV**).

## Build (desk)

```bash
cd sidekick
idf.py build
idf.py flash monitor
```

Target is pinned to `esp32s3` in `CMakeLists.txt`; no `set-target` step needed. The partition table is `partitions.csv`: two 3 MB OTA slots, no factory ([`sidekick.md`](../docs/architecture/sidekick.md) (**Firmware updates**)). With `otadata` erased the bootloader boots `ota_0`, which is what the ADV's USB-C bootstrap writes. Flash size is pinned to 8 MB in `sdkconfig.defaults` — without it the build takes ESP-IDF's 2 MB default and writes a wrong flash-size field into the bootloader header. `sdkconfig` is gitignored and regenerated, so delete it after changing the defaults or the old value sticks. `PROJECT_VER` is also pinned there — exact git SHA + dirty flag, not ESP-IDF's default `git describe` — so the ADV can compare an exact build identity ([`sidekick.md`](../docs/architecture/sidekick.md) (**Installing the sidekick from the ADV**)), not a fuzzy tag-relative string.

## Field-flash from the ADV

`../tools/stage_nano_firmware.sh` copies a build here's `build/` into `../components/nano_flasher/target_firmware/`, where the ADV build embeds it and the ADV can write it to a factory ("Green") Nano over its own USB-C host port. Proven on real hardware 2026-09-03.

**Before ever field-flashing a specific physical Nano, back it up** — this lets you re-green it afterward instead of the part being a one-shot:

```bash
esptool.py --chip esp32s3 -b 460800 \
    read_flash 0x0 0x800000 stock_backup/atoms3_stock_backup.bin
```

Note both changes from the NanoC6 form: `--chip esp32s3`, and `0x800000` because the AtomS3 Lite has 8 MB
where the Nano had 4 MB. Reading only the first 4 MB would produce a backup that silently restores garbage
over the upper half.

`../tools/restore_sidekick_stock.sh` writes it back afterward (port optional; it autodetects). `stock_backup/` is gitignored on purpose (vendor firmware, and a snapshot of one specific unit) — this is a personal safety net you (re)create per device, not a repo asset.

## Version identity

Every ESP-IDF app embeds an `esp_app_desc_t` (`project_name`, `version`) as the literal first bytes of its DROM segment — for this build, absolute flash offset **`0x20020`** (was `0x10020` before the OTA partition change moved the app from `factory` at `0x10000` to `ota_0` at `0x20000`). Re-derive if target, IDF version, partition table or secure boot changes — **re-verified after the esp32s3 retarget on 2026-09-08 and unchanged**: S3 and C6 both put the bootloader at `0x0` and the app at `0x10000`, and the descriptor is 0x20 into the image. Checked by reading the built `sidekick.bin`, not assumed. The ADV reads that off a Nano's flash before offering to install (`esp_loader_flash_read`, no app cooperation needed) to tell "not installed" from "installed, up to date" from "installed, needs updating" — and once `sidekick` is on PORTA as the daily companion link, it answers the same question at runtime via `esp_app_get_description()` in the handshake reply.

## Status

Serves the web app, provisioning and pairing, and relays between the browser and the main board over Port A. How it works and why: [`docs/architecture/sidekick.md`](../docs/architecture/sidekick.md).

- **Grove pins are GPIO1/GPIO2** (silkscreen `G`, `5V`, `G2`, `G1`; M5Stack labels Grove pins by GPIO number). With the straight-through cable the ADV receives on G1 and transmits on G2, so the sidekick transmits on G1 and receives on G2. Confirmed on hardware 2026-09-09. Neither is an S3 strapping pin (GPIO0/3/45/46).
- **Port A can never flash a stock chip (B42).** The ROM bootloader listens only on UART0's default pins (GPIO43/44 on S3), and Grove is GPIO1/GPIO2. USB-C is the only wire flash path; updates come over WiFi ([RFC 0007](../docs/rfcs/0007-flash-and-update.md)).
