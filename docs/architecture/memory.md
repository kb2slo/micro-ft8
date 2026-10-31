# Memory on the P4

Where the P4's RAM goes, who owns each large piece, and how a change adds to it without silencing the station. The board's facts (PSRAM on; the waterfall, decode working sets and capture path there): [main_board.md](main_board.md), **P4**. The decoder that owns most of it: [decode.md](decode.md). The sidekick's RAM is in [sidekick.md](sidekick.md).

## Adding a buffer

Read this before adding any buffer over about 1 KB to the P4 image.

1. **Long-lived and large: static, with one owner.** A buffer that lives as long as its feature runs goes in BSS. The linker places it or the build fails, it shows in `nm`, and heap fragmentation cannot take it away. Mark decode data for PSRAM with `DECODE_PSRAM_BSS` (`components/ft8_lib/common/decode_psram.h`); it lands in `.ext_ram.bss` and does **not** count in the internal-RAM budget.
2. **Small or short-lived: heap.** Allocate it where it is used and free it there. Prefer `MALLOC_CAP_INTERNAL` when the buffer must stay out of PSRAM; the build uses `CONFIG_SPIRAM_USE_CAPS_ALLOC`, so plain `malloc` stays internal.
3. **DMA buffers: internal RAM.** USB and other DMA targets stay in internal RAM ([main_board.md](main_board.md)), even though this P4's PSRAM is DMA-capable in silicon.
4. **No size-conditional fallback.** Never "use the static buffer if it fits, else `malloc`". When the configuration outgrows the static buffer, nothing fails: the buffer goes quiet and stays in the image. Refuse the configuration and log both sizes instead (`monitor_init()` does).
5. **No library that brings hidden statics.** A header can pull in far more than its call: check the budget's figure before and after. C++ streams (`<sstream>`, `<iostream>`) are out: any one of them links the C++ locale machinery. Format with `snprintf` and append to a `std::string` (`station_serialize()`).
6. **Static growth in internal RAM spends heap.** Internal statics and the heap share internal RAM. `tools/check_ram_budget.py` fails the build above `BUDGET_BYTES`, the one place that number lives; raising it is a decision recorded here, not a side effect of a change. PSRAM statics do not spend that budget. After SPIRAM (2026-10-31) internal statics were 176,756 bytes and `BUDGET_BYTES` was lowered to 200,000.
7. **Name the owner in the comment** and why it is static or heap (and internal or PSRAM), and keep it true. A stale comment naming a user that no longer links the file is how an unused buffer looks needed.

## Who owns the large pieces

Sizes come from the build: `check_ram_budget.py` prints the total, and `riscv32-esp-elf-nm -S --size-sort -C poe_p4/build/poe_p4.elf` the symbols.

| Piece | Where | Static or heap, and why |
| --- | --- | --- |
| The waterfall | `waterfall_static_buf`, `components/ft8_lib/common/monitor.c` | Static in PSRAM (`DECODE_PSRAM_BSS`). Sized by `MONITOR_WF_STATIC_BYTES` in `poe_p4/CMakeLists.txt` for the largest waterfall the P4 runs: FT8 at t2/f2. FT4 needs less, when the P4 runs it (row xph7). Host tools build without that define and `malloc` the waterfall to size, so they can run any configuration. |
| Decode working sets | `s_dec`, candidates, retry, SNR slot, decoded tables in `decode_pipeline.cpp` (and the same set in `test_apps/decode_bench`) | Static in PSRAM (`DECODE_PSRAM_BSS`). Used only by the CPU during find/decode. |
| The ingest ring | `s_ring_storage`, `components/ft8_audio_pipeline/ft8_audio_pipeline.cpp` | Static in PSRAM (`DECODE_PSRAM_BSS`). Holds audio while decode runs ([decode.md](decode.md), **The time budget**). The CPU copies into it, so no DMA reaches it. |
| FFT plan, window and frame buffers | `fft_work_static`, `window_static`, `last_frame_static`, `timedata`, `freqdata` in `monitor.c` | Static in PSRAM (`DECODE_PSRAM_BSS`), sized by `MONITOR_NFFT_MAX`. Touched every block, and PSRAM costs that about 0.6%: BDRY `mon` 323.0 against 324.8 ms a slot on 20 m (2026-10-31, [RFC 0014](../rfcs/0014-hl2-two-p4.md) §8). |
| Port A's outbound queue | `s_out`, `components/porta/porta.cpp` | Static, sized for a busy slot's frames (`porta_out_queue.h`). |
| A slot's decode results, two copies | `alloc_slot_result()`, `components/decode_pipeline/decode_pipeline.cpp` | Heap, internal (`MALLOC_CAP_INTERNAL`), on first use. |
| The heard-grid table | `table()`, `components/decode_pipeline/heard_grid.cpp` | Heap, on first use; null when there is no RAM, and then nothing is noted. |
| Task stacks | `xTaskCreate*` calls in `poe_p4/main` and `components` | Heap, at task creation. |
| Code in internal RAM | ESP-IDF's `*_IN_IRAM` placements | Not ours to size. It comes from the same memory as the data (`idf.py size` shows how much). |

## How it is checked

* **At build time:** `check_ram_budget.py` runs after every `poe_p4` build, local and CI, and prints the total and the 15 largest statics. It counts static **internal** DRAM only (`.dram0` / `.dram1`); PSRAM `.ext_ram.bss` is outside that figure, and the code in internal RAM is not in it either. A buffer nobody uses passes the budget, so read the list when a change adds to it.
* **On the device:** the boot log's `HEAP:` lines (`log_heap()`, `components/debug_log`) give free, minimum-free and largest-block figures at `AUDIO_PIPE_BEFORE_MONITOR_INIT`, `AUDIO_PIPE_AFTER_MONITOR_INIT` and `AUDIO_PIPE_AFTER_SAMPLE_BUFFERS`. Measured 2026-10-30 on `continuous-8-g47a542d`, QMX attached: `free` 172,264 at BEFORE_MONITOR_INIT and 165,848 at AFTER_SAMPLE_BUFFERS, with the largest block 90,112 and 86,016; `monitor_init()` takes 132 bytes of heap. After a QMX power-cycle the pipeline restarted with `free` 154,960 and the largest block 65,536: the heap fragments across reconnects, which the static waterfall does not need to survive.
* **Free heap and the largest block are different numbers.** Internal RAM is several regions (the boot log's `heap_init` lines), so the largest block is about half the free heap or less. A heap allocation over about 64 KB can fail with plenty free.

## Why not

* **The waterfall on the heap, allocated when audio starts.** It needed one contiguous block, and statics shrank the heap it came from, so a few KB of new statics stopped every decode at runtime, with nothing at build time (2026-10-24). Freed at each stop, the block was also split by small allocations made while the radio was away, and decoding did not restart after a power cycle. Kept for the program's life, it still hid from every build-time tool.
* **A static buffer for the small configuration, `malloc` for the large.** The firmware's configuration outgrew the static buffer and the `malloc` always ran, so 80,538 bytes of BSS sat unused in the image while docs described them as the decoder's arena.
* **C++ streams for parsing and formatting.** One `istringstream` in the decoder and one `ostringstream` in `station_serialize()` linked the C++ locale machinery: removing both took 4,624 bytes of statics and about 172 KB of flash out of the image (2026-10-30).
* **Freeing a large buffer to lend its space to another mode.** The same fragmentation: a small allocation lands in the freed hole before the next large one. Two modes that never run together share one static buffer, with one owner at a time.
