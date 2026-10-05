# Decode: how fast it runs, and how that is measured

What the decoder costs on each main board today, and the tool that measures it. The plan for improving decode — parameters, the audio path, algorithm changes — is [RFC 0006](../rfcs/0006-decode-performance.md), still in progress. The decoder itself is `ft8_lib` ([ft8_lib.md](ft8_lib.md)).

## The time budget

`ft8_audio_pipeline_run()` starts decoding after **79 blocks, 12.64 s** into the slot, inside the capture task, so decode must finish before the next boundary at 15.0 s: a **2.36 s guard**. That trigger point is a design choice, not a property of FT8 (RFC 0006 §4.3, row B90). The clock that matters is **candidate search plus decode** (`ftx_find_candidates()` and the per-candidate LDPC). The FFT runs as audio arrives (`monitor_process()`, ~2% of a core) and is not on that clock. Each receive slot sends that clock as a Port A log the sidekick prints: `SLOT <idx> c<found>/<cap> m<n> t<n> dec<ms> left<ms> y<ms> b<c0>/<c1>`. The capture task does not write it to the console UART; a line per decoded message there sat inside this timer. `dec` is `esp_timer`, sampled before the send, so a clock step in the same slot does not add to it. `left` is the soft clock's time until the slot boundary when decode returns, negative if decode ran past it. `y` is this slot's sync step in ms, 0 if none. `b` is each core's busy percent over this decode, core 0 then core 1: 100 means that core's idle task did not run. The counter is the FreeRTOS run-time stat (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` in `poe_p4/sdkconfig.defaults`), clocked by `esp_timer` at 1 µs, the same counter on both cores. A line longer than the 64-character event text is sent without `y`, so truncation cannot clip `b`. These lines are Port A logs; raising `CONFIG_LOG_MAXIMUM_LEVEL` compiles in every `ESP_LOGD` and is its own starvation. The SLOT line is also the browser debug log. The capture task sends `BDRY <idx> g<n> lost<n> drop<n>` the same way (`g` is blocks captured after the decode reset, about 14 for a full guard; `lost` is samples short of one slot at 6 kHz, 90,000 for FT8; `drop` is samples the ingest ring could not hold). A second task reads USB into a 16,384-sample ring while decode runs in the capture task, and a backlog is processed immediately instead of paced out to the next symbol. `SKIP <idx>` or `PAUSED <idx>` is sent the same way when a slot is not decoded. A transmitted slot is remembered by index (`tx_slot_idx`). `was_txing` clears when the tones end, about 10 ms after 12.64 s, the same moment the 79th block arrives, so the index is what the skip checks. Autoseq lines (`AUTOSEQ`) name the state (`calling`, `replying`, `report`, `roger`, `rogers`, `signoff`) and the next message (`tx1`…`tx5`).

## Build: the decoder runs at `-O2`

`ft8_lib` and `decode_pipeline` compile at `-O2` (`target_compile_options` in their `CMakeLists.txt`); the rest of the image stays at ESP-IDF's default `-Og`, so it remains debuggable. Decode output is byte-identical at both levels, so the change is pure speed: about **1.9× on the P4**, against 1.1–1.2× on a desktop, because in-order cores lose more to `-Og`. It was the largest single speed lever measured.

## Measured (2026-09-23/24)

`test_apps/decode_bench/` (its README has the commands) builds one C core and one configuration table into a device image and a host binary. It decodes a few corpus slots, stored in flash, under each configuration and prints the messages and the find+decode time. The P4 image uses `poe_p4/partitions.csv` and flashes the app slot only, so `nvs` and `storage` survive. Building is an agent's job; flashing and reading the console are the operator's.

Find + decode, mean over the five busiest corpus slots (27–33 `jt9` decodes each), worst slot in brackets, one core. The bench filled 80 blocks where the firmware uses 79, so these are ~1% slow; rerun before quoting better than a few percent.

| Configuration | Decodes (5 slots) | P4 `-O2` | Host `-O2` (Apple M4) |
|---|---|---|---|
| 50 candidates (the previous cap) | 72 | 315 ms (335) | 6.4 ms |
| freq_osr 1, 256 candidates | 83 | 1,870 ms (1,880) | 18.3 ms |
| Shipped (t2/f2, 256 candidates) | 96 | **1,747 ms** (1,790) | 16.3 ms |
| t4/f2, 256 candidates | 100 on host | allocation failed | 14.5 ms |

What these settle:

* **Host and device decode identically.** Every configuration and slot that ran, at both optimisation levels, produced exactly the host's messages, including frequency and DT. Sweeping decoder settings on the host is therefore sound for this decoder.
* **A desktop core is ~50× a P4 core on light work and ~100× on the LDPC-heavy candidate loop**. So `jt9`-class decoders do not fit on the P4 (RFC 0006 §4.1).
* **The P4 bench fits the shipped t2/f2 decode inside the guard on one core**, at 1,747 ms on the busiest slots. That is the search the firmware runs (`s_freq_osr` 2, `MONITOR_NFFT_MAX` 1920 on the P4), before the second subtraction pass below. The 161 KB waterfall is a malloc; the static arena stays 80,538 B. The FFT plan is static, sized for `MONITOR_NFFT_MAX`, so it does not split the one heap block that malloc needs (`components/ft8_lib/common/monitor.c`). The firmware heap took the waterfall (`waterfall=161076`).
* **Failing candidates are the cost.** Nearly all the time added by 256 candidates is LDPC spent on candidates that never decode.
* **Large waterfalls are a contiguous-RAM question.** t4/f2's 322 KB waterfall cannot allocate; it would need PSRAM, which the build leaves off ([main_board.md](main_board.md), **P4**). t2/f2's 161 KB fits the firmware heap.

## Recall against jt9

`python3 tools/decode_recall/report.py` scores the firmware's decode (the same steps as `ft8_audio_pipeline_run()` and `decode_monitor_results()`, via `test_apps/decode_bench` `bench_run_slot`) against pinned `jt9 -8 -d 3` truth from WSJT-X 3.0.1. Truth is committed under `tools/decode_recall/truth/`; CI does not run `jt9`. The match key is normalised message text. The report is recall, recall by `jt9`'s SNR, and the discordant counts (missed, and extras listed for review). Host wall time is printed and is not device time.

Two off-air corpora, both 12 kHz. The in-tree set is the 24 WAVs in `components/ft8_lib/vendor/test/wav` (428 messages). `20m_busy` is 38 slots (1,041 messages) fetched from `howard0su/ft8_lib` at `55ccf01` into a gitignored cache, not committed. A 6 kHz configuration — today's firmware — is fed a low-pass then 2:1 decimation in the harness, not the firmware's stride decimator. That aliasing question stays [RFC 0006](../rfcs/0006-decode-performance.md) P3. A 12 kHz configuration takes the WAVs as they are.

Flags select every shipped tunable: `time_osr`, `freq_osr`, candidates, `DEC_MAX`, `min_score`, LDPC iterations, sample rate, per-block AGC, waterfall levels, and subtraction. Levels below 256 quantise each cell onto that many codes across a 32 dB window anchored at the noise floor, one value per byte. That is the fewer-bits simulation, not packed storage.

The synthetic tier encodes two messages with the vendored GFSK synthesiser, adds white noise at a known SNR in a 2500 Hz bandwidth, and scores whether that message comes back. It is an absolute curve for this decoder. It is not a capture-path test: USB audio, the stride decimator, and whether a full slot arrived stay on the device (RFC 0006 §3).

CI runs the command on today's settings and fails if either corpus drops more than a few messages under the recorded hits (`tools/decode_recall/baseline/today.txt`), or if a synthetic signal at −6 dB or stronger does not decode. The slack is there because a marginal candidate can fall either way under a different libm. On the host that wrote the baseline, the firmware settings below recalled 285/428 and 699/1041, and the synthetic signals were solid from −18 dB up.

## Subtraction

After each message decodes, the firmware re-encodes it and lowers those tone bins in the waterfall, then searches again. The amount removed is half the estimated SNR (the mean tone level minus the mean of the per-symbol minimum of the other tones). That minimum runs low, so a full subtraction digs a hole; on these corpora full subtraction lost a true message and half did not. The same lowering is applied at the decoded `time_sub` and the neighbouring one, which at `time_osr` 2 is the other half of each symbol, and at every `freq_sub`. A candidate within 4 Hz of one already decoded in that pass is skipped, so the other frequency subdivision of the same signal is not subtracted twice. The second pass asks for half as many candidates (`256 / (pass + 1)`).

Discordant counts are true `jt9` messages gained and lost against the previous firmware (t2/f2, 256 candidates, one search). No file filled `DEC_MAX` 32. Host find+decode is per slot over both corpora, Apple M4, `-O2`. It ranks configurations; it is not device time.

| Config | In-tree | Busy | Discordant | Host find+decode |
|---|---|---|---|---|
| One search (previous firmware) | 274/428 | 674/1041 | — | 16.2 ms |
| 2 passes, half, decoded `time_sub` only | 284/428 | 695/1041 | +10/−0, +21/−0 | 19.3 ms |
| **2 passes, half, neighbouring `time_sub` (firmware)** | **285/428** | **699/1041** | **+11/−0, +25/−0** | **18.8 ms** |
| 3 passes, half, neighbouring `time_sub` | 286/428 | 702/1041 | +12/−0, +28/−0 | 24.3 ms |
| 2 passes, full SNR, neighbouring `time_sub` | 284/428 | 696/1041 | +10/−0, +23/−1 | — |

A quarter of the SNR was worse than half (279/428 and 689/1041 with neighbours). Busy-set recall in the weakest `jt9` bin moved 26/63 → 27/63. The messages gained sit higher: −3..0 dB went 72/132 → 79/132, and ≥ 1 dB went 383/477 → 398/477. Extras, not scored as false, stayed 4 on the in-tree set and went 8 → 10 on the busy set. The one-search row is itself +65/−0 and +169/−0 against the older 50-candidate, `freq_osr` 1 firmware. Both corpora favour the decoder their author tuned, so a win on them is expected, not proof.

The P4 bench for the one-search t2/f2 decode is 1,747 ms. Scaling this host's find+decode ratio (18.8 / 16.2) puts two passes near 2.0 s, about 0.3 s more, inside the 0.45–0.8 s of air guard measured on 20 m the same day. Three passes scale to about 0.9 s more, which overruns that guard, so they are not in the firmware. The scaled figure is not a device measurement. The field check is in [TEST_PLAN.md](../TEST_PLAN.md).

## Measuring decode yield on the air

The bench measures time. Any change claiming to decode *more* is measured on the air, this way, not by impression:

* **The metric is unique decodes per slot.** Not total rows.
* **A/B at the same time of day, never the same evening.** Band conditions move faster than any change we would make.
* **Build the comparison version in a separate worktree**, so the two arms are not one tree at two times.
* **Full chip erase before each flash.** NVS carried over has changed behaviour between arms before.
* **60 slots (15 minutes) per arm.** Report the mean and the standard deviation, not the mean alone.
* **Change one thing at a time.**

The standard deviation is what stops a false result. In one independent project on this decoder, two arms averaged 15.38 decodes per slot each, with σ 5.70 against 6.65: a suspected regression that was band fading.
