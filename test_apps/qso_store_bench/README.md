# qso_store_bench

Times what the callsign card and its neighbours would ask of the QSO store (`components/qso_store/`, epic
[I32](../../docs/roadmap/qso-store.md)), on the P4 and on the host, before the store is linked into the
firmware. Nothing here touches flash: the journal is built in a PSRAM buffer, so `load` is the CPU half of a
boot (checking every entry and rebuilding the index), not the read from flash.

`main/bench_core.cpp` fills a store with 1,000, 8,000 and 19,000 synthetic QSOs written by the firmware's own
`adif_format_log_record()`, so the bytes per record are the logger's. The same records every run: three years
of dates, a third as many stations as QSOs, picked with a skew so a few are worked often and most once or
twice. **heavy** is the most-worked station, a worst case (133, 598 and 1,035 QSOs); **typical** is the median
QSO's station (5 QSOs). A `CHECK` line per size compares each answer with what the generator wrote.

| Operation | What it is |
| --- | --- |
| `size` | Journal bytes, and bytes per record |
| `add` | One `qso_store_add()`, the append itself free (the bytes are already in RAM) |
| `load` | `qso_store_load()` over the whole journal |
| `by_call_*` | The card: the call table's probe, then `qso_store_get()` for each of that call's records. `_probe` is the probe alone |
| `worked_band_mode` | `qso_store_worked()` on a band and mode, as a decode would ask it |
| `worked_at_park` | The same during an activation, which reads each of the call's records |
| `select_date`, `select_unsent` | `qso_store_select()`: one pass over the index |
| `scan_grid_get` | A field with no index: every record read through `qso_store_get()`, as a grid search would be today |
| `scan_grid_raw` | The floor under that scan: one `memmem` pass over the journal's bytes |

## Host

```bash
make -C test_apps/qso_store_bench/host && test_apps/qso_store_bench/host/qso_store_bench_host
```

## Device

Build inside the ESP-IDF environment, from `test_apps/qso_store_bench`, at the firmware's `-Og`. The store's
buffers are static in PSRAM, as the firmware's would be: a 6 MB journal, 20,480 index entries and 65,536 call
slots.

```bash
idf.py -B build set-target esp32p4 build
python ../decode_bench/tools/run_on_device.py auto build/qso_store_bench.bin p4.log
```

The runner writes the app slot `ota_0` only and erases `otadata`, leaving the bootloader, `nvs` and `storage`
alone ([decode_bench](../decode_bench/README.md), **Device**). The firmware's app is overwritten: reflash it
afterwards (`cd poe_p4 && idf.py build flash`). A P4 restart can leave the QMX unenumerated until it is switched
off and on (row [f8u2](../../docs/ROADMAP.md#f8u2)).

## Results

Microseconds per operation, except `size`. Host: x86-64 Linux, `-Og` (2026-11-25, `qso-store-link`). P4: U213, 360 MHz,
PSRAM at 200 MHz (2026-11-25, the same tree).

| Operation | n=1,000 host | n=8,000 host | n=19,000 host | n=19,000 P4 |
| --- | --- | --- | --- | --- |
| bytes per record | 209.2 | 209.4 | 209.5 | 209.5 |
| `add` | 6.2 | 6.4 | 6.3 | 344 |
| `load` | 3,466 | 28,541 | 68,521 | 3,624,828 |
| `by_call_heavy` (133 / 598 / 1,035 QSOs) | 278 | 1,285 | 2,258 | 116,522 |
| `by_call_heavy_probe` | 0.4 | 2.4 | 4.5 | 628 |
| `by_call_typical` (5 QSOs) | 11.5 | 11.2 | 10.6 | 536 |
| `by_call_miss_probe` | 0.0 | 0.0 | 0.0 | 0.5 |
| `worked_band_mode` | 0.3 | 1.4 | 3.0 | 528 |
| `worked_at_park` | 338 | 1,555 | 2,672 | 161,442 |
| `select_date` | 0.8 | 6.0 | 15.1 | 7,318 |
| `select_unsent` | 1.6 | 13.1 | 30.8 | 8,842 |
| `scan_grid_get` | 2,165 | 17,319 | 41,810 | 2,173,085 |
| `scan_grid_raw` | 22 | 175 | 407 | 133,270 |
