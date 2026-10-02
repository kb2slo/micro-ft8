# Decode: how fast it runs, and how that is measured

What the decoder costs on each main board today, and the tool that measures it. The plan for improving decode — parameters, the audio path, algorithm changes — is [RFC 0006](../rfcs/0006-decode-performance.md), still in progress. The decoder itself is `ft8_lib` ([ft8_lib.md](ft8_lib.md)).

## The time budget

`ft8_audio_pipeline_run()` starts decoding after **79 blocks, 12.64 s** into the slot, inside the capture task, so decode must finish before the next boundary at 15.0 s: a **2.36 s guard**. That trigger point is a design choice, not a property of FT8 (RFC 0006 §4.3, row B90). The clock that matters is **candidate search plus decode** (`ftx_find_candidates()` and the per-candidate LDPC). The FFT runs as audio arrives (`monitor_process()`, ~2% of a core) and is not on that clock.

## Build: the decoder runs at `-O2`

`ft8_lib` and `decode_pipeline` compile at `-O2` (`target_compile_options` in their `CMakeLists.txt`); the rest of the image stays at ESP-IDF's default `-Og`, so it remains debuggable. Decode output is byte-identical at both levels, so the change is pure speed: about **1.9× on the P4**, against 1.1–1.2× on a desktop, because in-order cores lose more to `-Og`. It was the largest single speed lever measured.

## Measured (2026-09-23/24)

`test_apps/decode_bench/` (its README has the commands) builds one C core and one configuration table into a device image and a host binary. It decodes a few corpus slots, stored in flash, under each configuration and prints the messages and the find+decode time. The P4 image uses `poe_p4/partitions.csv` and flashes the app slot only, so `nvs` and `storage` survive. Building is an agent's job; flashing and reading the console are the operator's.

Find + decode, mean over the five busiest corpus slots (27–33 `jt9` decodes each), worst slot in brackets, one core. The bench filled 80 blocks where the firmware uses 79, so these are ~1% slow; rerun before quoting better than a few percent.

| Configuration | Decodes (5 slots) | P4 `-O2` | Host `-O2` (Apple M4) |
|---|---|---|---|
| Shipped (t2/f1, 50 candidates) | 72 | **315 ms** (335) | 6.4 ms |
| 256 candidates | 83 | 1,870 ms (1,880) | 18.3 ms |
| t2/f2, 256 candidates | 96 | 1,747 ms (1,790) | 16.3 ms |
| t4/f2, 256 candidates | 100 on host | allocation failed | 14.5 ms |

What these settle:

* **Host and device decode identically.** Every configuration and slot that ran, at both optimisation levels, produced exactly the host's messages, including frequency and DT. Sweeping decoder settings on the host is therefore sound for this decoder.
* **A desktop core is ~50× a P4 core on light work and ~100× on the LDPC-heavy candidate loop**. So `jt9`-class decoders do not fit on the P4 (RFC 0006 §4.1).
* **The P4 fits 256 candidates and t2/f2 inside the guard on one core**, with ~0.5 s to spare on the busiest slots.
* **Failing candidates are the cost.** Nearly all the time added by 256 candidates is LDPC spent on candidates that never decode.
* **Large waterfalls are a contiguous-RAM question.** t4/f2's 322 KB waterfall cannot allocate; it would need PSRAM, which the build leaves off ([main_board.md](main_board.md), **P4**). t2/f2's 161 KB fit in the bench images but must be re-checked inside the firmware, which has far less free heap.

## Measuring decode yield on the air

The bench measures time. Any change claiming to decode *more* is measured on the air, this way, not by impression:

* **The metric is unique decodes per slot.** Not total rows.
* **A/B at the same time of day, never the same evening.** Band conditions move faster than any change we would make.
* **Build the comparison version in a separate worktree**, so the two arms are not one tree at two times.
* **Full chip erase before each flash.** NVS carried over has changed behaviour between arms before.
* **60 slots (15 minutes) per arm.** Report the mean and the standard deviation, not the mean alone.
* **Change one thing at a time.**

The standard deviation is what stops a false result. In one independent project on this decoder, two arms averaged 15.38 decodes per slot each, with σ 5.70 against 6.65: a suspected regression that was band fading.
