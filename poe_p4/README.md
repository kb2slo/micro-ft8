# poe_p4

M5Stack Unit PoE-P4 (SKU U213, ESP32-P4) firmware: the headless main board, and the daily station. Board facts, ports and power: [`docs/architecture/main_board.md`](../docs/architecture/main_board.md) (**P4**). Planned work: [`docs/ROADMAP.md`](../docs/ROADMAP.md).

Decodes, transmits, runs autoseq and keeps the log, with the QMX on the Host USB-C port and the sidekick on Grove; the phone is the only UI. Most of that is shared code in `components/`. `poe_p4/main/` holds the headless entry point, the QMX USB transport, the TX scheduler, and the Port A host actions.

Separate `idf.py` project from `sidekick`: ESP-IDF locks target *and* sdkconfig per project, and this is a different chip (ESP32-P4).

## Build (desk)

```bash
cd poe_p4
idf.py build
idf.py flash monitor
```

Target is pinned to `esp32p4` in `CMakeLists.txt`; no `set-target` step needed. Flash size is pinned to 16 MB in `sdkconfig.defaults` (the U213's actual flash) — without it the build takes ESP-IDF's default and writes a wrong flash-size field into the bootloader header, same reasoning `sidekick/sdkconfig.defaults` gives. `sdkconfig` is gitignored and regenerated, so delete it after changing the defaults or the old value sticks. PSRAM is not yet enabled — see the comment in `sdkconfig.defaults`.

**Partition table (I29):** `partitions.csv` — custom LittleFS `storage` (~3 MB) for `Station.txt` / `.adi`, plus a 4 MB factory app slot. Same rule as flash size: if `sdkconfig` still has `CONFIG_PARTITION_TABLE_SINGLE_APP`, delete `sdkconfig` and reconfigure or the default 1 MB single-app table stays and LittleFS fails with `partition "storage" could not be found`.
