# decode_bench

docs/architecture/decode.md. Times the firmware's decode steps on the same corpus slots on every board, and checks
the device decodes exactly what the host does. There is no radio or USB audio, only the decoder.

What it runs is `main/bench_core.c`: the firmware's per-block AGC and `monitor_process()` (as
`ft8_audio_pipeline_run()` does), then `ftx_find_candidates()` / `ftx_decode_candidate()` / unpack (as
`decode_monitor_results()` does). The configurations are in `main/bench_configs.h`, and the device image and
host binary share that one table.

## 1. Slots (once)

```bash
python3 test_apps/decode_bench/tools/make_slots.py
```

This writes `data/slot_0..4.raw`: the five busiest corpus slots, at 6 kHz, from the in-tree set and from
`howard0su/ft8_lib@55ccf01` `20m_busy`, fetched and never committed. `data/` is gitignored.

## 2. Host (parity reference)

```bash
make -C test_apps/decode_bench/host && test_apps/decode_bench/host/decode_bench_host test_apps/decode_bench/data/slot_*.raw > host.log
```

## 3. Device

Build inside the ESP-IDF environment, from `test_apps/decode_bench`. `-Og` is how the firmware ships, and
`-O2` is docs/architecture/decode.md, Build:

```bash
idf.py -B build_og -D SDKCONFIG=build_og/sdkconfig set-target esp32p4 build
idf.py -B build_o2 -D SDKCONFIG=build_o2/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.o2" set-target esp32p4 build
```

Flash **the app slot only** and capture the output:

```bash
python tools/run_on_device.py /dev/cu.usbmodemXXXX build_o2/decode_bench.bin p4_O2.log
```

The P4 build uses `poe_p4/partitions.csv`, so writing only `0x10000` leaves the bootloader, `nvs` and the
`storage` partition (Station.txt) alone. The firmware's app is overwritten, so reflash it afterwards.

### ADV (ESP32-S3)

Build the same two images with `set-target esp32s3`, into `build_s3_og` and `build_s3_o2`
(`sdkconfig.defaults.esp32s3`: 240 MHz, 8 MB flash, console on USB-Serial-JTAG, and the repo `partitions.csv`,
which is the firmware's native layout). The Cardputer needs button presses, because a software reset after
flashing lands it back in download mode:

1. Put it in bootloader mode (hold G0 while resetting or replugging).
2. Flash: `python -m esptool --chip esp32s3 --port PORT --before no_reset --after no_reset write_flash 0x10000 build_s3_o2/decode_bench.bin`
   (or `--full` through `run_on_device.py` to write the bench bootloader and partition table too, on a unit
   without Launcher).
3. Start the reader before resetting:
   `python tools/run_on_device.py auto build_s3_o2/decode_bench.bin s3_O2.log --chip esp32s3 --capture-only --timeout 1800`
   (`auto` follows the port if a replug renames it; `1101` became `101` on 2026-09-24).
4. Reset normally, without G0. The bench waits 3 s before printing so the reader can reconnect.

Launcher units have no factory app slot for this. Use an OTA slot as `tools/flash_keep_launcher.py` does, never
`0x10000`.

## Reading the output

- `RESULT <config> <slot> ... find_ms decode_ms total_ms process_ms`
  - `total_ms` (find + decode) is what must fit the 2.36 s FT8 guard band.
  - `process_ms` is capture-time CPU, spread across the slot.
- `MSG <config> <slot> <Hz> <DT> <text>`: diff these against the host log for parity.
- `ok=0` means the monitor could not allocate, for example a waterfall bigger than the free heap.
