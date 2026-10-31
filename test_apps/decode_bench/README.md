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
`-O2` is docs/architecture/decode.md, Build. The default image puts the waterfall and decode working
sets in PSRAM, as the firmware does:

```bash
idf.py -B build_og -D SDKCONFIG=build_og/sdkconfig set-target esp32p4 build
idf.py -B build_o2 -D SDKCONFIG=build_o2/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.o2" set-target esp32p4 build
```

Internal-RAM A/B (same `-O2`, the waterfall, decode working sets and capture path in DRAM) to re-check PSRAM cost:

```bash
idf.py -B build_o2_internal -D SDKCONFIG=build_o2_internal/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.o2;sdkconfig.internal_ram" \
  set-target esp32p4 build
```

Flash **the app slot only** and capture the output:

```bash
python tools/run_on_device.py /dev/cu.usbmodemXXXX build_o2/decode_bench.bin p4_O2_psram.log
python tools/run_on_device.py /dev/cu.usbmodemXXXX build_o2_internal/decode_bench.bin p4_O2_internal.log
```

Diff the `MSG` lines for identical decodes; compare `RESULT` `total_ms` (and the `BENCH` line's
`wf=psram|internal`) for whether PSRAM slowed the search, and `process_ms` for whether it slowed the capture
path. The bench runs the monitor back to back with a warm cache; the firmware's BDRY `mon` times it in place
(docs/architecture/decode.md).

The P4 build uses `poe_p4/partitions.csv`, so writing only `0x10000` leaves the bootloader, `nvs` and the
`storage` partition (Station.txt) alone. The firmware's app is overwritten, so reflash it afterwards.

## Reading the output

- `RESULT <config> <slot> ... find_ms decode_ms total_ms process_ms`
  - `total_ms` (find + decode) is what must fit the 2.36 s FT8 guard band.
  - `process_ms` is capture-time CPU, spread across the slot.
- `MSG <config> <slot> <Hz> <DT> <text>`: diff these against the host log for parity.
- `ok=0` means the monitor could not allocate, for example a waterfall bigger than the free heap.
