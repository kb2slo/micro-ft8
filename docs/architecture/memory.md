# Memory on the P4

Where the P4's RAM goes, who owns each large piece, and how a change adds to it without silencing the station. The board's facts (PSRAM fitted but off): [main_board.md](main_board.md), **P4**. The decoder that owns most of it: [decode.md](decode.md). The sidekick's RAM is in [sidekick.md](sidekick.md).

## Adding a buffer

Read this before adding any buffer over about 1 KB to the P4 image.

1. **Long-lived and large: static, with one owner.** A buffer that lives as long as its feature runs goes in BSS. The linker places it or the build fails, it shows in `nm` and in the budget check at its real size, and heap fragmentation cannot take it away.
2. **Small or short-lived: heap.** Allocate it where it is used and free it there.
3. **DMA buffers: internal RAM.** They cannot use PSRAM ([main_board.md](main_board.md)), which is off anyway.
4. **No size-conditional fallback.** Never "use the static buffer if it fits, else `malloc`". When the configuration outgrows the static buffer, nothing fails: the buffer goes quiet and stays in the image. Refuse the configuration and log both sizes instead (`monitor_init()` does).
5. **No library that brings hidden statics.** A header can pull in far more than its call: check the budget's figure before and after.
6. **Static growth spends heap.** Statics and the heap share internal RAM. `tools/check_ram_budget.py` fails the build above `BUDGET_BYTES`, the one place that number lives; raising it is a decision recorded here, not a side effect of a change.
7. **Name the owner in the comment** and why it is static or heap, and keep it true. A stale comment naming a user that no longer links the file is how an unused buffer looks needed.

## Who owns the large pieces

Sizes come from the build: `check_ram_budget.py` prints the total, and `riscv32-esp-elf-nm -S --size-sort -C poe_p4/build/poe_p4.elf` the symbols.

| Piece | Where | Static or heap, and why |
| --- | --- | --- |
| The waterfall | `waterfall_static_buf`, `components/ft8_lib/common/monitor.c` | Static. Sized by `MONITOR_WF_STATIC_BYTES` in `poe_p4/CMakeLists.txt` for the largest waterfall the P4 runs (FT8 at t2/f2; FT4 needs less). Host tools build without that define and `malloc` the waterfall to size, so they can run any configuration. |
| The ingest ring | `s_ring_storage`, `components/ft8_audio_pipeline/ft8_audio_pipeline.cpp` | Static. Holds audio while decode runs ([decode.md](decode.md), **The time budget**). |
| FFT plan, window and frame buffers | `fft_work_static`, `window_static`, `last_frame_static`, `timedata`, `freqdata` in `monitor.c` | Static, sized by `MONITOR_NFFT_MAX`. |
| Port A's outbound queue | `s_out`, `components/porta/porta.cpp` | Static, sized for a busy slot's frames (`porta_out_queue.h`). |
| A slot's decode results, two copies | `alloc_slot_result()`, `components/decode_pipeline/decode_pipeline.cpp` | Heap, on first use. |
| The heard-grid table | `table()`, `components/decode_pipeline/heard_grid.cpp` | Heap, on first use; null when there is no RAM, and then nothing is noted. |
| Task stacks | `xTaskCreate*` calls in `poe_p4/main` and `components` | Heap, at task creation. |
| Code in internal RAM | ESP-IDF's `*_IN_IRAM` placements | Not ours to size. It comes from the same memory as the data (`idf.py size` shows how much). |

## How it is checked

* **At build time:** `check_ram_budget.py` runs after every `poe_p4` build, local and CI. It counts static DRAM only; the code in internal RAM comes from the same memory and is not in its figure.
* **On the device:** the boot log's `HEAP:` lines (`log_heap()`, `components/debug_log`) give free, minimum-free and largest-block figures at `AUDIO_PIPE_BEFORE_MONITOR_INIT`, `AUDIO_PIPE_AFTER_MONITOR_INIT` and `AUDIO_PIPE_AFTER_SAMPLE_BUFFERS`. No reading has been taken since the waterfall went static ([TEST_PLAN.md](../TEST_PLAN.md), **Pending**).

## Why not

* **The waterfall on the heap, allocated when audio starts.** It needed one contiguous block, and statics shrank the heap it came from, so a few KB of new statics stopped every decode at runtime, with nothing at build time (2026-10-24). Freed at each stop, the block was also split by small allocations made while the radio was away, and decoding did not restart after a power cycle. Kept for the program's life, it still hid from every build-time tool.
* **A static buffer for the small configuration, `malloc` for the large.** The firmware's configuration outgrew the static buffer and the `malloc` always ran, so 80,538 bytes of BSS sat unused in the image while docs described them as the decoder's arena.
* **Freeing a large buffer to lend its space to another mode.** The same fragmentation: a small allocation lands in the freed hole before the next large one. Two modes that never run together share one static buffer, with one owner at a time.
