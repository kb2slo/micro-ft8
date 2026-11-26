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
| `add` | 3.5 | 3.7 | 3.4 | 171 | 172 |
| `load` | 1,377 | 10,829 | 25,164 | 56,273 | 1,042,018 |
| `by_call_*` | The card: the call table's probe, then `qso_store_get()` for each of that call's records. `_probe` is the probe alone |
| `worked_band_mode` | 0.4 | 1.6 | 2.9 | 20.0 | 528 |
| `worked_at_park` | 60.0 | 253 | 417 | 3,474 | 30,129 |
| `select_date`, `select_unsent` | `qso_store_select()`: one pass over the index |
| `scan_grid_get` | 79.0 | 623 | 1,511 | 7,570 | 153,489 |
| `scan_grid_raw` | 24.3 | 167 | 402 | 5,258 | 99,723 |

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

| Operation | n=1,000 host | n=8,000 host | n=19,000 host | n=1,000 P4 | n=19,000 P4 |
| --- | --- | --- | --- | --- | --- |
| bytes per record | 209.2 | 209.4 | 209.5 | 209.2 | 209.5 |
| `add` | 3.5 | 3.7 | 3.4 | 171 | 172 |
| `load` | 1,377 | 10,829 | 25,164 | 56,273 | 1,042,018 |
| `by_call_heavy` (133 / 598 / 1,035 QSOs) | 3.5 | 13.9 | 29.8 | 247 | 6,559 |
| `by_call_heavy_probe` | 0.6 | 2.4 | 4.5 | 29.9 | 629 |
| `by_call_typical` (5 QSOs) | 0.1 | 0.2 | 0.1 | 9.7 | 11.3 |
| `by_call_miss_probe` | 0.0 | 0.0 | 0.0 | 0.5 | 0.5 |
| `worked_band_mode` | 0.4 | 1.6 | 2.9 | 20.0 | 528 |
| `worked_at_park` | 60.0 | 253 | 417 | 3,474 | 30,129 |
| `select_date` | 0.8 | 5.7 | 14.4 | 54.5 | 7,308 |
| `select_unsent` | 1.6 | 12.4 | 30.6 | 115 | 8,856 |
| `scan_grid_get` | 79.0 | 623 | 1,511 | 7,570 | 153,489 |
| `scan_grid_raw` | 24.3 | 167 | 402 | 5,258 | 99,723 |
