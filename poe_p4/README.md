# poe_p4

M5Stack Unit PoE-P4 (SKU U213, ESP32-P4) firmware — the headless main-board port tracked as `I27` in [`docs/ROADMAP.md`](../docs/ROADMAP.md), scoped in [`docs/rfcs/0005-p4-poe-headless-port.md`](../docs/rfcs/0005-p4-poe-headless-port.md) ("RFC 0005"). Do not duplicate the plan here.

**Status: Phase 0 skeleton only.** This project builds and boots (`ESP_LOGI` heartbeat, nothing else) to prove `idf.py build` targets `esp32p4` cleanly. No radio, no decode, no UI, no companion link yet — those are RFC 0005's later phases.

Separate `idf.py` project from both the ADV app (`micro_ft8`, root `CMakeLists.txt`) and `sidekick` — ESP-IDF locks target *and* sdkconfig per project, so a board on a different chip entirely (ESP32-P4, not the ADV's/sidekick's ESP32-S3) cannot share either project's configure, same reasoning `sidekick/CMakeLists.txt` and `sidekick/README.md` already give for why the sidekick isn't folded into the ADV's own project.

**Shared code, not duplicated (RFC 0005 §5):** as radio control, the decode/autoseq/TX pipeline, and the sidekick protocol client get ported, they belong in `components/` — the same place `porta_proto` already lives as a component shared between the ADV and sidekick projects via `EXTRA_COMPONENT_DIRS` — not copied into `poe_p4/main/`. Nothing has been extracted yet; this skeleton has no shared-component dependency at all.

## Build (desk)

```bash
cd poe_p4
idf.py build
idf.py flash monitor
```

Target is pinned to `esp32p4` in `CMakeLists.txt`; no `set-target` step needed. Flash size is pinned to 16 MB in `sdkconfig.defaults` (the U213's actual flash, per RFC 0005 §4) — without it the build takes ESP-IDF's default and writes a wrong flash-size field into the bootloader header, same reasoning `sidekick/sdkconfig.defaults` gives. `sdkconfig` is gitignored and regenerated, so delete it after changing the defaults or the old value sticks. PSRAM is not yet enabled — see the comment in `sdkconfig.defaults`.

**Partition table (I29):** `partitions.csv` — custom LittleFS `storage` (~3 MB) for `Station.txt` / `.adi`, plus a 4 MB factory app slot. Same rule as flash size: if `sdkconfig` still has `CONFIG_PARTITION_TABLE_SINGLE_APP`, delete `sdkconfig` and reconfigure or the default 1 MB single-app table stays and LittleFS fails with `partition "storage" could not be found`.
