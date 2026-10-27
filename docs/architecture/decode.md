# Decode: how fast it runs, and how that is measured

What the decoder costs on each main board today, the tool that measures it, and how the SNR it reports is held to WSJT-X's. The plan for improving decode — parameters, the audio path, algorithm changes — is [RFC 0006](../rfcs/0006-decode-performance.md), still in progress. The decoder itself is `ft8_lib` ([ft8_lib.md](ft8_lib.md)).

## The time budget

`ft8_audio_pipeline_run()` starts decoding after **79 blocks, 12.64 s** into the slot, inside the capture task, so decode must finish before the next boundary at 15.0 s: a **2.36 s guard**. That trigger point is a design choice, not a property of FT8 (RFC 0006 §4.3, row B90). The clock that matters is **candidate search plus decode** (`ftx_find_candidates()` and the per-candidate LDPC). The FFT runs as audio arrives (`monitor_process()`, ~2% of a core) and is not on that clock. Each receive slot sends that clock as a Port A log the sidekick prints: `SLOT <idx> c<found>/<cap> m<n> t<n> dec<ms> left<ms> y<ms> b<c0>/<c1>`. The capture task does not write it to the console UART; a line per decoded message there sat inside this timer. `dec` is `esp_timer`, sampled before the send, so a clock step in the same slot does not add to it. `left` is the soft clock's time until the slot boundary when decode returns, negative if decode ran past it. It is read after this slot's clock step, so a line with `y320` reads `left` 320 ms lower than the decode's real margin. `y` is this slot's sync step in ms, 0 if none. The step is the negated median start time of the slot's decodes, when there are at least four and the median is past 0.3 s, capped at 320 ms (`decode_sync.h`). Only a clean slot steps the clock: one boundary was crossed into it, the slot before it was whole to within one block, and the ingest ring dropped nothing from the start of that slot to this decode. A clock set or step cuts a slot short, so the slot after one is not used. `b` is each core's busy percent over this decode, core 0 then core 1: 100 means that core's idle task did not run. The counter is the FreeRTOS run-time stat (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` in `poe_p4/sdkconfig.defaults`), clocked by `esp_timer` at 1 µs, the same counter on both cores. A line longer than the 64-character event text is sent without `y`, so truncation cannot clip `b`. These lines are Port A logs; raising `CONFIG_LOG_MAXIMUM_LEVEL` compiles in every `ESP_LOGD` and is its own starvation. The SLOT line is also the browser debug log. The capture task sends `BDRY <idx> g<n> lost<n> drop<n>` the same way (`g` is blocks processed after the decode reset, about 14; `lost` is samples short of one slot at 6 kHz, 90,000 for FT8; `drop` is samples the ingest ring could not hold). A second task reads USB into a 16,384-sample ring while decode runs in the capture task. The capture task processes each block as soon as its last sample is in the ring, sleeping one tick (10 ms) only when the ring is empty, so decode starts within about a tick of its 79th block's audio. Each block is dated by its audio: the soft clock minus the samples queued behind it (`ft8_block_slot_idx()`). So when a decode runs past the boundary, the queued tail still belongs to its own slot, and the next slot's waterfall starts on its own audio; `left` is what shows the overrun, and `g` stays near 14. Before this, the boundary was the clock when a block was processed, and blocks were paced to a 160 ms grid; with dated blocks that pacing cost about 0.27 s of guard on 15 m (mean `dec`+`left` 2.31 s before, 2.04 s after) until it was removed (2.28 s over the next 35 slots). A 15 s slot is 93.75 blocks, so where the first block falls in the slot cycles through four positions 40 ms apart, and the margin with it: 2.2–2.37 s on that run. A slot with few messages still decodes in about 2.1–2.3 s, so on a quiet band a decode can end up to about 0.1 s past the boundary; with blocks dated by their audio that costs the next slot nothing (`lost` 720 at most, `drop` 0). On 15 m (2026-10-05) a decode past the boundary then started the next waterfall about 2 s early, decode times alternated between about −0.4 s and +1.7 s, the early slot's own decode overran in turn, and syncing from both stepped the clock +320 then −320 ms on alternate slots. The ring rejects new samples when full, so after a drop the oldest queued audio is dated late by the gap; such a slot is not clean. `SKIP <idx>` or `PAUSED <idx>` is sent the same way when a slot is not decoded. A transmitted slot is remembered by index (`tx_slot_idx`). `was_txing` clears when the tones end, about 10 ms after 12.64 s, the same moment the 79th block arrives, so the index is what the skip checks. Autoseq lines (`AUTOSEQ`) name the state (`calling`, `replying`, `report`, `roger`, `rogers`, `signoff`) and the next message (`tx1`…`tx5`).

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

`python3 tools/decode_recall/report.py` scores the firmware's decode (the same steps as `ft8_audio_pipeline_run()` and `decode_monitor_results()`, via `test_apps/decode_bench` `bench_run_slot`) against pinned `jt9 -8 -d 3` truth from WSJT-X 3.0.1. Truth is committed under `tools/decode_recall/truth/`; CI does not run `jt9`. The match key is normalised message text. The report is recall, recall by `jt9`'s SNR, the discordant counts (missed, and extras listed for review), and how far our SNR sits from `jt9`'s (**SNR** below). Host wall time is printed and is not device time.

Two off-air corpora, both 12 kHz. The in-tree set is the 24 WAVs in `components/ft8_lib/vendor/test/wav` (428 messages). `20m_busy` is 38 slots (1,041 messages) fetched from `howard0su/ft8_lib` at `55ccf01` into a gitignored cache, not committed. A 6 kHz configuration — today's firmware — is fed a low-pass then 2:1 decimation in the harness, not the firmware's stride decimator. That aliasing question stays [RFC 0006](../rfcs/0006-decode-performance.md) P3. A 12 kHz configuration takes the WAVs as they are.

Flags select every shipped tunable: `time_osr`, `freq_osr`, candidates, `DEC_MAX`, `min_score`, LDPC iterations, sample rate, per-block AGC, waterfall levels, and subtraction. Levels below 256 quantise each cell onto that many codes across a 32 dB window anchored at the noise floor, one value per byte. That is the fewer-bits simulation, not packed storage.

The synthetic tier encodes two messages with the vendored GFSK synthesiser, adds white noise at a known SNR in a 2500 Hz bandwidth, and scores whether that message comes back. It is an absolute curve for this decoder. It is not a capture-path test: USB audio, the stride decimator, and whether a full slot arrived stay on the device (RFC 0006 §3).

CI runs the command on today's settings and fails if either corpus drops more than a few messages under the recorded hits (`tools/decode_recall/baseline/today.txt`), if a synthetic signal at −6 dB or stronger does not decode, or if the SNR drifts (**SNR** below). The slack is there because a marginal candidate can fall either way under a different libm. On the host that wrote the baseline, the firmware settings below recalled 285/428 and 699/1041, and the synthetic signals were solid from −18 dB up.

## Subtraction

After each message decodes, the firmware re-encodes it and lowers those tone bins in the waterfall, then searches again. The amount removed is half the estimated SNR (the mean tone level minus the mean of the per-symbol minimum of the other tones). That minimum runs low, so a full subtraction digs a hole; on these corpora full subtraction lost a true message and half did not. The same lowering is applied at the decoded `time_sub` and the neighbouring one, which at `time_osr` 2 is the other half of each symbol, and at every `freq_sub`. A candidate within 4 Hz of one already decoded in that pass is skipped, so the other frequency subdivision of the same signal is not subtracted twice. The second pass asks for half as many candidates (`256 / (pass + 1)`). The second search skips a candidate that already failed at the same position when no subtracted signal overlaps its tone bins (`decode_retry.h`). A decode reads only its candidate's tone bins, and a subtraction lowers only the decoded signal's, so that candidate would fail again. A search that subtracts nothing is the last. Both corpora and eight pure-noise slots decode exactly the same messages. Host find+decode fell 36% on slots with no messages, 24% with 1–9, 12% with 10–19 and 8% with 20 or more. Failed candidates run every LDPC iteration and decoded ones stop early, so a slot with few messages costs more than a busy one. On the P4 (15 m, 2026-10-05), a slot with no messages took 2,152 ms with `left` positive; on 40 m (2026-10-02), before the search stopped, it took 3.27 s. A slot with one message took about 3.2 s before the retry skip. With the skip, on 15 m (2026-10-05), slots of 6–9 messages decoded in 2.1–2.3 s, and with the antenna off empty slots took 2,145–2,152 ms.

Discordant counts are true `jt9` messages gained and lost against the previous firmware (t2/f2, 256 candidates, one search). No file filled `DEC_MAX` 32. Host find+decode is per slot over both corpora, Apple M4, `-O2`. It ranks configurations; it is not device time.

| Config | In-tree | Busy | Discordant | Host find+decode |
|---|---|---|---|---|
| One search (previous firmware) | 274/428 | 674/1041 | — | 16.2 ms |
| 2 passes, half, decoded `time_sub` only | 284/428 | 695/1041 | +10/−0, +21/−0 | 19.3 ms |
| **2 passes, half, neighbouring `time_sub` (firmware)** | **285/428** | **699/1041** | **+11/−0, +25/−0** | **18.8 ms** |
| 3 passes, half, neighbouring `time_sub` | 286/428 | 702/1041 | +12/−0, +28/−0 | 24.3 ms |
| 2 passes, full SNR, neighbouring `time_sub` | 284/428 | 696/1041 | +10/−0, +23/−1 | — |

A quarter of the SNR was worse than half (279/428 and 689/1041 with neighbours). Busy-set recall in the weakest `jt9` bin moved 26/63 → 27/63. The messages gained sit higher: −3..0 dB went 72/132 → 79/132, and ≥ 1 dB went 383/477 → 398/477. Extras, not scored as false, stayed 4 on the in-tree set and went 8 → 10 on the busy set. The one-search row is itself +65/−0 and +169/−0 against the older 50-candidate, `freq_osr` 1 firmware. Both corpora favour the decoder their author tuned, so a win on them is expected, not proof.

The P4 bench for the one-search t2/f2 decode is 1,747 ms. Scaling this host's find+decode ratio (18.8 / 16.2) puts two passes near 2.0 s, about 0.3 s more, inside the 0.45–0.8 s of air guard measured on 20 m the same day. Three passes scale to about 0.9 s more, which overruns that guard, so they are not in the firmware. On the P4, two passes on busy 20 m slots read `dec` 1.6–2.0 s (2026-10-05).

## SNR

Every SNR the station shows or sends — the `R` line, the web app's decode list, the report autoseq transmits, the log's RST — is `decode_snr_message()` (`components/decode_pipeline/decode_snr.c`). It is on WSJT-X's scale: the signal against the noise in 2500 Hz, in whole dB, floored at −24 as `jt9` is, and capped at +49, the most a report can carry (WSJT-X's `ft8code` reads a sent +55 as −46). WSJT-X's method, built here from its description rather than its code:

* **Signal:** the mean power in the decoded message's tone cell, over every symbol the waterfall holds.
* **Noise:** each frequency column's mean power over the slot; at the signal, the 10th percentile of those means within ±400 Hz of its middle tone. On a busy band the columns other stations occupy sit above that percentile.
* **The AGC divided back out.** `ft8_audio_pipeline_run()` scales each 160 ms block to a fixed level before the waterfall, and passes those gains to the decode. Each cell is divided by the gain² under its analysis frame: the four half-blocks ending at that cell, weighted by the Hann window squared. Without this, the hot gain before a busy band's stations start, or while a strong one fades, inflates the noise. Frames that reach back before the slot are not measured: on the device they hold the previous slot's audio.
* **The scale:** `10·log10(signal/noise − 1) + DECODE_SNR_FT8_OFFSET_DB`, −26.1 dB, fitted on a synthetic grid that `jt9` reads within 0.2 dB of its injected SNR. It is close to the expected value: a 320 ms Hann window has a 4.7 Hz noise bandwidth, which gives −27.3 dB, and a 160 ms symbol fills only part of that window. FT4 uses FT8's constant moved by the wider bandwidth of its shorter symbol, not fitted (row xph7).

Measured with `report.py` (2026-10-10): the synthetic grid reads its injected SNR within 0.8 dB from −18 to +20 dB, and exactly from −10 dB up. Against `jt9` on the messages both decoded, our SNR minus theirs is mean +0.1 dB, sd 3.3 (in-tree, 285 messages) and +1.5, sd 2.9 (busy, 699), with slopes 0.94 and 1.01. The estimate this replaced (one waterfall cell minus the slot's 25th-percentile cell) read a median 16 dB above `jt9`, with slope 0.44. CI fails if either corpus's mean error moves more than 0.5 dB from the recorded value or its sd grows more than 0.5 dB, or if the synthetic grid reads more than 1.5 dB off at any level from −18 dB up; a change to the waterfall or the method must refit the constant. On the host the SNR costs 44 µs per slot and 4.5 µs per message; it adds 2.1 KB of static RAM and a gain per block on the heap.

Why not:

* **A median over time** instead of the mean: off-air it read about 6 dB high. Real band noise is not steady, and `jt9` averages.
* **The mean without dividing the AGC out:** strong signals compressed (+20 dB read +15 on the synthetic grid), and the off-air sd rose to 4.0 dB.
* **One noise level for the whole band** (a global percentile): sd 5.7 dB off-air. The passband and a band's occupancy are not flat.
* **The signal's own unused tones as the noise:** sd 10.5 dB. GFSK transitions and neighbouring stations leak into them.
* **Summing the neighbouring frequency sub-bins, or a baseline per tone:** no change in the result. The baseline is taken once, at the middle tone, which is 8× cheaper.

## Band activity

While a slot is received, each 160 ms block (48 ms in FT4) also yields one number for the page's slot bar ([webapp.md](webapp.md), **The slot bar's activity trace**): about how many signals the column it just added to the waterfall holds (`ft8_block_activity()`). A bin is active at 10 dB or more above that column's median (`FT8_ACTIVITY_THRESHOLD_CODES`), and active bins with at most two inactive bins between them (`FT8_ACTIVITY_GAP_BINS`) are one signal. It reads the first time and frequency subdivision only. The AGC scales a whole block, so a level measured against the median does not move with it. Noise alone passes about 0.1% of bins. A tone 10 dB up in one bin is about −16 dB SNR in 2500 Hz, so the weakest stations the decoder finds do not show, and signals too weak to decode do.

**Why clusters, not bins.** A station's tone spreads over about four 6.25 Hz bins, more when it is strong, so a count of active bins measured how loud the band was more than how many were on it. The first build counted bins: on 20 m (2026-10-27) slots of 21–23 decodes read 85–110 in every block. On the 38 slots of the recall corpora (`tools/decode_recall`, with `jt9`'s decode count as the reference), bins read about 30 at 10 decodes and 100–130 at 20–34. Clusters read about 13 at 10 decodes and 23–30 at 20–34: close to the station count up to about 25, then flat, because signals that close merge. A gap of one bin and of three gave the same counts. It costs a pass over about 432 codes per block, in the capture task, and a 512-byte histogram on its stack: no static RAM.

**The time axis says little on a busy band.** Every FT8 station transmits for the whole 12.6 s, so after the rise at about 0.5 s the level is flat until the signals end. What it carries is how busy the band is, and when the signals start and stop.

**Dated and batched like the decode.** A block's place in its slot comes from the audio-dated clock (`ft8_block_pos()`, the same dating as `ft8_block_slot_idx()`). Levels go out in batches of about a second (`ft8_activity_batch_*`: 7 FT8 blocks, 21 FT4 blocks) as one `ACTIVITY` Port A event, so the sidekick's 64-row event ring takes about one row a second more. A batch also closes at a new slot, at a skipped position, and just before the decode trigger, because the decode holds the capture task for about 2 s. The blocks queued during that decode are then processed in a burst, so on the page the first ~2 s of each slot arrive about 2 s late. Not computed while we transmit (`tx_active` or `was_txing`): that audio is not the band. Host-tested by `host_test_block_activity`. The page's scale and the threshold are set from the corpora and one live session, not yet checked across bands (TEST_PLAN, Pending).

## Measuring decode yield on the air

The bench measures time. Any change claiming to decode *more* is measured on the air, this way, not by impression:

* **The metric is unique decodes per slot.** Not total rows.
* **A/B at the same time of day, never the same evening.** Band conditions move faster than any change we would make.
* **Build the comparison version in a separate worktree**, so the two arms are not one tree at two times.
* **Full chip erase before each flash.** NVS carried over has changed behaviour between arms before.
* **60 slots (15 minutes) per arm.** Report the mean and the standard deviation, not the mean alone.
* **Change one thing at a time.**

The standard deviation is what stops a false result. In one independent project on this decoder, two arms averaged 15.38 decodes per slot each, with σ 5.70 against 6.65: a suspected regression that was band fading.
