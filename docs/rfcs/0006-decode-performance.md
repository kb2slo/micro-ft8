# RFC 0006: Decode performance — measure, then tune

* **Status:** Draft. Opened 2026-09-23 to stage the decode-performance work that had spread across four
  roadmap rows ([B25](../ROADMAP.md), [B53](../ROADMAP.md), [B54](../ROADMAP.md), [I26](../ROADMAP.md)) plus
  three new ones ([B87](../ROADMAP.md), §5 P0; [B85](../ROADMAP.md), §5 P3; [B86](../ROADMAP.md), §5 P6). Those rows now carry an ID, a name and a
  link here; the reasoning lives in this file.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** how decode quality is measured, the corpus and its ground truth, the host harness, what is
  tunable on each board, and the order in which parameter, audio-path and algorithm changes are tried.
* **Does not cover:** the P4 port itself ([RFC 0005](0005-p4-poe-headless-port.md)), the UI of the decode
  list, or the QSO sequencer.
* **Ask:** confirm the phase order in §5 before starting. P0 (device benchmark) and P1 (host harness) are the
  candidates for Now.

---

## 1. Why

We run the `ft8_lib` decoder below its own author's defaults, and we cannot tell whether any change to it is
an improvement. External measurements put the gap to WSJT-X at roughly **30%** of spots for stock `ft8_lib`
(the Web-888 skimmer's starting point: under 70% of WSJT-X's spots) and around **2 dB** for another
independent decoder (pancetta: +2.1 dB to reach 50% recall on FT8). A single-sample comparison published by
the `ft8ts` author decoded 8 messages with `ft8_lib` against 14 for WSJT-X. Nobody has measured ours.

Two things changed the calculus in September 2026:

- **The P4 lifts the memory ceiling.** RFC 0005 §4 confirms 32 MB of octal PSRAM on the U213 (not yet
  enabled in the P4 build), and its internal RAM alone holds what the S3 cannot (§4.2). The settings §4 below
  proves unreachable on the ESP32-S3 become a matter of measurement, not arithmetic.
- **Usable corpora already exist.** `components/ft8_lib/vendor/test/wav/` holds 22 scored 15-second slots
  (362 WSJT-X decodes, 12 kHz mono) plus a scorer, `utils/run_tests.py`, and `howard0su/ft8_lib` publishes 38
  busy 20 m slots. `jt9`, the WSJT-X
  command-line decoder, is enough to regenerate truth without the GUI. So the first tuning round needs no
  recording at all.

## 2. What we measure, and what truth means

**Primary metric: recall against pinned `jt9`.** `jt9 -8 -d 3 file.wav` at a pinned WSJT-X version
(3.0.1 at time of writing). The version and depth are stored with every truth file. Truth from an unrecorded
decoder version is not truth: the in-tree `.txt` files were produced by an older WSJT-X, and `jt9` 3.0.1
already finds one more decode than they list on each of the two files spot-checked (19 vs 18 on
`websdr_test1`, 6 vs 5 on `191111_110130`).

**Match key:** normalised message text (hashed callsigns collapsed to `<...>`, as `run_tests.py` does). We do
not match on frequency or time, because the two decoders report them with different resolution.

**Also reported per run:**

- recall bucketed by `jt9`'s SNR (the curve, not just the total);
- decodes only we produce ("extras"), listed for review rather than scored as false, because a real signal
  WSJT-X missed and a false decode look identical to the scorer;
- host decode wall-time, as a relative CPU cost between configurations. Host time is never evidence about
  on-device timing.

**Comparing two configurations** uses the *discordant* decodes only — those one configuration found and the
other missed on the same file. With 362 truth decodes, a total-recall difference of a couple of points is
noise; a discordant count that runs 15–2 one way is not. Every result table states the discordant counts.

**Truth is tiered, and WSJT-X is not the top tier.** Scoring against another decoder caps recall at 100% of
that decoder. Each corpus file records the provenance of its truth, and the scorer reports per tier:

1. **Synthetic** — we encoded it, so recall and precision are exact. Calibrated AWGN gives an **absolute**
   recall-vs-SNR curve against FT8's published ≈ −21 dB threshold.
2. **Synthetic through a fading channel** (Watterson-style).
3. **Synthetic injected over recorded band noise** — makes congestion a dial, which is the right experiment
   for `num_candidates` and `DEC_MAX`.
4. **Real off-air, scored against `jt9`** — the first round's tier.
5. **Real off-air, scored by consensus across independent decoders** (e.g. `jt9` + JTDX) — the only tier
   that can score *above* WSJT-X.

Synthesis trap: `synth_fsk()` in `tests/tx_e2e/synth.c` is plain FSK without Gaussian pulse shaping, so it
is an easier signal than a real one and flatters any decoder measured on it. Tiers 1–3 use the vendored GFSK
synthesiser on the host, where its large stack VLAs (`dphi` in `synth_gfsk()` ~622 KB at 12 kHz) are
harmless.

## 3. Boundaries: what the host can and cannot prove

**Testing the decoder given audio is a different claim from testing the capture path that delivers it.**
The host harness proves the first and says nothing about the second: not USB/UAC timing, not DRAM, not
whether a full slot of audio arrived. `TEST_PLAN.md`'s field-only rule stands unchanged. The capture path is
§5 P4 and is measured on hardware.

**One check licenses all host-only sweeps.** Host and device are not bit-identical — different FPU and
libm, and LDPC is iterative and thresholded, so a marginal candidate can fall either way. Before any
host-picked configuration ships, run ~20 corpus files on the target device and confirm the decode sets
match. If they do, the host is a valid proxy and later sweeps are free. If they don't, that is worth knowing
before building on the assumption.

**Host ranks; the device decides feasibility.** Decode must finish inside the guard band — FT8: 79 symbols
× 0.160 s = 12.64 s of signal in a 15 s slot, leaving **2.36 s**; FT4: 105 × 0.048 = 5.04 s in 7.5 s,
leaving **2.46 s**. The host can say which configuration decodes more. Only a timed run on the device can
say whether it fits.

**Timing is measured on the device, never scaled from paper.** On paper, ESP32-S3 (ADV, 240 MHz Xtensa LX7)
against ESP32-P4 (360 MHz RISC-V as configured in `poe_p4/sdkconfig`), both dual-core with single-precision
FPUs, is a clock ratio of 1.5× and an unknown IPC ratio. That is too coarse when §4.1's best candidate is
within a small factor of the guard. And paper cannot model what differs most: PSRAM latency on a
waterfall sweep, cache behaviour, and `ft8_lib`'s static DIRAM arena against a PSRAM one.

So the device benchmark is its own small deliverable, §5 P0 ([B87](../ROADMAP.md)). Same input on both
boards, no radio, no USB audio. It serves four purposes at once: it replaces §4.1's guessed desktop-to-P4
factor with a measured one, it is the host/device parity check above, it ranks ADV against P4 on identical
work, and it is the A/B bench for §4.2's toolchain levers. The per-slot `SLOT … dec=…ms` line that
both firmwares already log from the shared `decode_pipeline` complements it with real-band timing, but it
cannot compare boards, because no two slots carry the same traffic.

## 4. What is tunable, per board

Current values, by symbol (verified 2026-09-23; cite by symbol, line numbers rot):

| Parameter | Ours | kgoba demo default | Where |
|---|---|---|---|
| `time_osr` | 2 | 2 | `s_time_osr` in `components/decode_tx_state/decode_tx_state.cpp` |
| `freq_osr` | **1** | 2 | `s_freq_osr`, same file |
| candidates | **50** | 140 | `max_cand` in `decode_monitor_results()`, `components/decode_pipeline/decode_pipeline.cpp` |
| `min_score` | 5 | 10 | literal on the `ftx_find_candidates()` call, same function |
| LDPC iterations | 25 | 25 | literal on the `ftx_decode_candidate()` call, same function |
| max decodes kept | **32** | 50 | `DEC_MAX` = `RX_MAX_DECODES`, `components/decode_types/include/decode_types.h` |

kgoba's values are the `k*` constants at the top of `components/ft8_lib/vendor/demo/decode_ft8.c`. We are
below him on three of six. Our `min_score` is already *more* permissive than his.

**`DEC_MAX` is the cheapest item and is not a DSP limit.** The candidate loop stops at
`s_dec_count < DEC_MAX`, so a busy slot is truncated by the size of the RX list, not by the decoder.

### ESP32-S3 (ADV): what is out of reach, and why

The waterfall is `max_blocks × time_osr × freq_osr × num_bins` bytes (`WF_ELEM_T` is `uint8_t`). It is a
static arena, `waterfall_static_buf`, in `components/ft8_lib/common/monitor.c`, sized for exactly today's
settings: 93 × 2 × 1 × 433 = **80,538 B**. The kiss_fftr arena beside it is 12,288 B.

- **`freq_osr` = 2 at `time_osr` = 2 needs 161,076 B** against about 94 KB of link-time DIRAM remainder. It
  is short by roughly 67 KB before runtime stacks. Every saving identified (on-demand USB host ~6 KB, USB
  Drive ~1.5 KB, going headless ~13.4 KB static plus ~8.6 KB framebuffer) sums to about 25 KB. It does not
  close, for a structural reason: `libft8_lib.a` alone is 108,212 B of DIRAM, 44% of the 247 KB used. The
  decoder *is* the memory. The measurements behind these numbers are in git history (2026-09-04).
- **The equal-cost swap is reachable.** `time_osr` = 1, `freq_osr` = 2 costs the same 80,538 B, trading time
  resolution for frequency resolution. `nfft` doubles to 1920, which pushes kiss_fftr past its 12,288 B
  arena into a ~20 KB `malloc`, so that arena needs ~8 KB more BSS to stay static.
- **Candidates are cheap on memory** (a stack array) and cost CPU only.
- **Neither ADV board has PSRAM** (confirmed with `esptool.py flash_id`, 2026-09-04; RFC 0001 §2). The
  waterfall is CPU-swept data, never a DMA target, so it *would* be a legitimate PSRAM candidate on a board
  that had it.
- **No other slower memory helps either** (checked 2026-09-23). PSRAM on this family is a second die in the
  package, so it cannot be retrofitted to an `FN8` part. RTC fast memory is 8 KB and already on the heap
  (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y`). Flash is out: the waterfall is rewritten every 80 ms, and
  flash writes are slow, wear the part, and stall the cache under live USB audio. The instruction cache is
  already at its 16 KB minimum. Shrinking the data cache from 32 KB to 16 KB frees about 16 KB, but on a board
  without PSRAM that cache serves flash-resident code and constants, `ft8_lib`'s LDPC tables among them, so it
  would slow the decoder to buy memory that still does not close the gap.

#### Fewer bits per cell: the one S3 route to t2/f2

The waterfall stores more precision than the signal carries. Each cell is a `uint8` in 0.5 dB steps over a
fixed −120..0 dB window (`monitor_process()` in `components/ft8_lib/common/monitor.c`). But a noise-only bin
fluctuates by about **5.6 dB** standard deviation from one reading to the next: that is the spread of an
exponentially distributed power expressed in dB, 10/ln 10 × π/√6. Against that, 0.5 dB steps add about
0.14 dB of rounding error, and even 2 dB steps add only about 0.58 dB.

So **4 bits per cell** (16 levels, about 2 dB apart, anchored at the noise floor) plausibly costs almost no
sensitivity. At 4 bits, **t2/f2 is 161,076 × ½ = 80,538 B, exactly today's arena.** The S3 would get the P4's
headline setting at zero memory delta.

That reasoning is an argument, not a result. It could fail on strong-signal clipping, on the candidate
search's bin-to-bin sync differences, or on SNR reporting, which is a percentile over the waterfall. Three
further constraints:

1. **The scale must be relative to the noise floor.** A fixed −120..0 window spends most of 16 levels on
   dB values no signal reaches. The per-block AGC in `ft8_audio_pipeline_run()` already pins the floor near
   a known level, which is what makes an anchored 4-bit scale possible. That AGC is also one of I26's audio-path
   suspects, so the two are tested together.
2. **Simulating is cheap; packing is not.** Every read of a cell goes through `WF_ELEM_MAG()` /
   `WF_ELEM_MAG_INT()` in `vendor/ft8/decode.h`, so quantising values to 16 levels while keeping one per byte
   is a change in `monitor.c` alone. Real two-per-byte packing is harder: `vendor/ft8/decode.c` also walks
   the buffer with pointer arithmetic (`p8[sm]`, `block_stride` offsets in the sync-score loops, and
   `get_cand_mag()`). That means an accessor through the vendored decoder, which is a divergence from the
   pin and belongs under [RFC 0002](0002-extract-and-boundaries.md) §6's pin discipline.
3. **Unpacking costs CPU** in the sync-score and likelihood loops. P0 measures it.

**Order:** simulate on the host in P2 first. Only if recall holds against 8-bit t2/f2 is packing worth
building.

### ESP32-P4 (PoE-P4): what opens up

32 MB PSRAM and a faster core make t2/f2, candidates ≥ 140, `DEC_MAX` 50+, and more LDPC iterations
memory-feasible. The static arena in `monitor.c` is sized for today's settings and must become
board-dependent before any of them run on the P4. The P4 build has PSRAM switched off today, and t2/f2
still fits in its internal RAM (§4.2), so PSRAM latency only matters for configurations larger than that. An independent `ft8_lib`-based decoder already runs `freq_osr` = 2 in production on a
P4-class part (SteffenLav's `qmx-panadapter`), which makes the S3 ceiling a practical loss rather than a
theoretical one.

**Algorithm changes** from the Web-888 skimmer work, very likely published as `howard0su/ft8_lib` (commit
history and target hardware match the rx-888.com write-up, not confirmed by either party):

- **OSD** — ordered-statistics LDPC decoding, a couple of dB over belief propagation, at materially more
  memory and CPU;
- **subtract and re-decode** — subtract each decoded signal's level in the frequency domain, then search
  again, which lets weaker signals under strong ones through at almost no memory cost;
- **message-plausibility filtering** of false decodes.

Their own recall/precision/F1 table puts the combined gain at about 7 points over stock `ft8_lib`. These are
P4-first. OSD may never fit the S3.

### 4.1 Replacing `ft8_lib`, measured

Measured 2026-09-23 on an Apple M4 (one core), truth `jt9 -8 -d 3` from WSJT-X 3.0.1, matched on message text.
Two corpora: the in-tree 24 12-kHz WAVs (428 messages) and `howard0su/ft8_lib`'s `test/wav/20m_busy` (38 busy
20 m slots, 1,041 messages). Every number below reproduces from a fresh clone with
`tools/decode_bench/build_variants.sh OUT_DIR`, which builds each variant from a pinned source plus one named
edit and then scores them all. Host CPU ranks configurations; it is never device timing (§3).

| Decoder | Recall, 428 set | Recall, 1,041 set | Host CPU per slot | License |
|---|---|---|---|---|
| kgoba `ft8_lib`, demo defaults (t2/f2, 140 candidates) | 65.7% | 65.8% | ~13 ms | MIT |
| `howard0su/ft8_lib` `55ccf01`, as published | 72.9% | 72.2% | ~90–120 ms | MIT |
| `jt9 -d 1` | 350 messages, 82% of depth 3's count (not matched) | — | ~440 ms | GPL-3.0 |
| `jt9 -d 3` | reference | reference | ~1,450 ms | GPL-3.0 |

#### What `howard0su/ft8_lib` is

42 commits on kgoba's 2023-10 release (`50ee0c0`). All of the FT8 decoder work landed 2024-12-18..22; 2025–26
added FST4/FST4W and memory hardening. **With every feature switched off it decodes exactly what kgoba does**
(281 matched, the same 4 extras on the 428 set), so the whole gain is in five features, each a switch. It
still compiles as C despite commit `218e47f` ("move to C++ compiler"), and newlib provides the GNU-order
`qsort_r` its OSD calls on non-Apple targets. Its `20m_busy` corpus is the busy-band data our in-tree corpus
lacks, and P1 adopts it.

**Leave-one-out** (base: the fork with the subtraction fix below). Host CPU is on the 1,041 set:

| Feature removed | Recall, 428 set | Recall, 1,041 set | Δ (1,041) | Host CPU | Memory the feature costs |
|---|---|---|---|---|---|
| none | 72.9% | 72.3% | — | ~87 ms | — |
| subtraction | 69.6% | 68.8% | **−3.5** | ~65 ms | none (in place on the waterfall) |
| `time_osr` 4 → 2 | 69.6% | 70.6% | −1.7 | ~124 ms | waterfall ×2 |
| OSD | 72.0% | 71.0% | −1.3 | **~24 ms** | ~40 KB of stack per call |
| 3 passes → 1 | 71.5% | 71.4% | −0.9 | ~46 ms | none |

- **OSD is the worst value in the fork.** It buys 1.3 points for ~3.6× the CPU and adds false decodes
  (extras 12 → 17). It is order-1 only: after BP fails with ≤ 32 parity errors (`kMaxLDPCErrors`), it tries
  flipping each of the 6 least reliable of the 91 most reliable bits, one at a time. WSJT-X's `osd174_91`
  goes to order 3 with a-priori bits. Each call runs a Gauss-Jordan elimination over `b[174][182]`, one bit
  per byte, about 3 M byte operations, with `b` and `gen1_inv[91][91]` (~40 KB) on the stack. Our FT8 decode
  task stack is 8 KB. Bit-packing the matrix into words would cut both the memory and the time by a large
  factor, if OSD is ever wanted. The header comments follow Robert Morris's MIT-licensed `ft8mon`.
- **Subtraction is the best value.** It is also crude: per symbol, it lowers only the decoded tone's own bin,
  at the candidate's own `time_sub`, in each `freq_sub`. Energy the same signal leaves in neighbouring
  `time_sub` positions is untouched, so there is room to improve it.
- **Passes only pay with subtraction.** Each pass re-runs the candidate search on the subtracted waterfall with
  `256 / (pass + 1)` candidates, skipping frequencies already decoded.
- The "fake signal filter" commit (`a2d9889`) drops messages that fail to unpack. `decode_monitor_results()`
  already does that (`urc != FTX_MESSAGE_RC_OK`), so there is nothing to take.

#### A portability bug: the subtraction is undefined behaviour

On the default `uint8` waterfall, the fork's `ft8/decode.h` defines `SUB_WF_ELEM_MAG(x, y)` as `(x) -= y * 2 + 240`. `y` is an SNR, which is a *difference* of dB values, so the
`+ 240` offset (which belongs only to absolute levels) is wrong. The result is always negative, and converting a
negative `float` to `uint8_t` is undefined in C. Measured:

| Target | Code generated | What subtraction does | Recall, 1,041 set |
|---|---|---|---|
| arm64, clang `-O2`/`-O3` | `fcvtzs`, low byte stored | subtracts (SNR − 8 dB); wraps to a large value below 0 | 72.2% |
| ESP32-P4, `riscv32-esp-elf-gcc` 14.2 `-O2` | `fcvt.wu.s` (saturates to 0) + `andi 0xff` | sets the tone bin to −120 dB | 71.0% |
| ESP32-S3, `xtensa-esp32s3-elf-gcc` 14.2 `-O2` | `utrunc.s` + `extui` | depends on `utrunc.s`'s negative-input result; unconfirmed | ? |

An explicit, clamped `x − 2y + 16` reproduces the arm64 result exactly on the 428 set and to within one
message on the 1,041 set (753 against 752). The clamp removes the wrap-around cases, which is the only place
they can differ, so this confirms the mechanism. Explicit full subtraction scores 71.5% / 72.0%, explicit half
subtraction 72.9% / 72.4%, and explicit zeroing 70.8% / 71.0% (428 / 1,041). **So a partial subtraction is the better algorithm, and the fork found it by accident.** The likely
reason is that the noise estimate (the minimum over the other tones) is biased low, which inflates the SNR.

Ported as published, the P4 would run the zeroing variant and land ~1.3 points under the host's numbers. §3's
parity check exists to catch exactly this. **Any port replaces the macro with an explicit, clamped subtraction
whose fraction is a named, tuned parameter.** Reported upstream as [howard0su/ft8_lib#2](https://github.com/howard0su/ft8_lib/pull/2) (2026-09-23; the repo has issues disabled): the behaviour-preserving clamped `x − 2y + 16`, measured with the fork's own `utils/run_tests.py` at 75.0% → 75.1% recall on arm64, against 73.5% for the saturate-to-0 behaviour RISC-V builds get today.

#### What our own settings cost

Stand-in for today's firmware: the fork's code at our values (t2/f1, 50 candidates, 1 pass, no subtraction, no
OSD). This runs 12 kHz WAVs without our 6 kHz stride decimation or per-block AGC, so it shows direction and
size, not our firmware's number; P1 measures that (and P3 explains why the real number may be lower). Building
up, host CPU on the 1,041 set:

| Step | Recall, 428 set | Recall, 1,041 set | Host CPU | Fits the S3? |
|---|---|---|---|---|
| stand-in for today (t2/f1, 50 candidates) | **50.2%** | **52.3%** | ~6 ms | yes |
| + 256 candidates | 54.4% | 57.4% | ~12 ms | **yes**: stack and CPU only |
| + subtraction + 3 passes | 56.1% | 58.8% | ~20 ms | **yes**: no memory |
| instead, the t1/f2 swap (same memory as t2/f1) | 58.9% | 58.6% | ~22 ms | yes; the corpora disagree (+2.8 / −0.2), so it stays in P2 |
| **+ `freq_osr` 2 (t2/f2)** | **68.7%** | **69.1%** | ~22 ms | only with §4's 4-bit waterfall |
| + `time_osr` 4 | 72.0% | 71.0% | ~24 ms | P4 only |
| + OSD | 72.9% | 72.3% | ~87 ms | P4 only, at ~3.6× the CPU |

Three conclusions:

1. **The 50-candidate cap binds and is the cheapest fix we have**, on both boards: +4.2 / +5.1 points for
   CPU only. B53's `cand=` field would show it in the field, but the host already does.
2. **`freq_osr` 2 is the largest single lever (+12.6 / +10.3 points).** That makes the 4-bit waterfall (§4) the
   deciding question for the ADV, not a nice-to-have.
3. **For the P4, subtraction + passes + t4/f2 without OSD reaches ~71%** at ~2× kgoba's host CPU. OSD is
   optional and comes last.

#### Porting WSJT-X itself is not feasible

For three independent reasons:

1. **CPU.** `jt9` spends ~0.44–1.45 s per slot on a desktop core. Even at an optimistic 30× slowdown to a P4
   core, that is 13–45 s against a 2.36 s guard band. Double-buffering (B54) does not rescue it: replying in
   the next slot still needs the decode inside roughly the guard. The 30–50× factor is a guess until P0
   replaces it, but the margin is an order of magnitude, not a rounding error.
2. **Language.** It is Fortran, and the ESP-IDF toolchains have no Fortran front end. A port is a hand
   translation. `e04/ft8ts` shows that is doable (TypeScript), not that it is cheap.
3. **License.** WSJT-X is GPL-3.0. Translated code is a derivative work, so the firmware would ship under
   GPL-3.0 with source owed. Our MIT base allows that, but it is one-way, and Wei's MIT tree could no longer
   take our decoder changes. Reimplementing from the published algorithm description (Franke, Somerville and
   Taylor, "The FT4 and FT8 Communication Protocols", QEX 2020) avoids it: algorithms are not copyrightable
   and code is. This is the project's reading, not legal advice.

#### What to take, in order

1. **Candidates up, on both boards.** The cap, not the decoder, is the first thing losing signals.
2. **Subtraction (fixed and tuned) plus passes, on both boards.** No memory; the fork's algorithm with the
   macro made explicit.
3. **t2/f2**: on the P4 directly; on the S3 only if the 4-bit waterfall holds.
4. **`time_osr` 4 on the P4.**
5. **A-priori (AP) decoding, written from the QEX paper.** During a QSO the decoder already knows most of the
   next message: our call, the partner's call, the expected report. WSJT-X fixes those bits before LDPC and
   gains several dB exactly in the QSO case. Our sequencer always has that context; a skimmer does not.
   Nothing in `howard0su/ft8_lib` does this.
6. **OSD last and optional**, and only bit-packed.

This is a feature list, not a library swap. Items 1–4 are small, separable changes to the decode path we
already vendor, which keeps one decoder for both boards and makes each change scoreable on its own. Taking the
fork wholesale would buy OSD and FST4 we may not want, a C++-leaning build, and the subtraction bug.

### 4.2 Hardware and toolchain levers

`ft8_lib` is portable C, and we build it the way ESP-IDF builds everything by default. What the tree shows
(2026-09-23):

- **Both targets compile at `-Og`.** `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` in `sdkconfig` and
  `poe_p4/sdkconfig`, and `components/ft8_lib/CMakeLists.txt` does not override it. That is ESP-IDF's
  default, not a decision anyone made.
- **The FFT is `kiss_fft`**, generic C. `esp-dsp`, Espressif's library with FFTs tuned for both chips, is not
  a dependency.
- **No hidden `double`.** The hot paths in `vendor/ft8/decode.c`, `vendor/ft8/ldpc.c` and `common/monitor.c`
  are `float` throughout (`log10f`, `fast_tanh`/`fast_atanh`). That matters because the S3 emulates `double`
  in software.
- **The FFT is not on the decode clock.** `monitor_process()` runs once per block as audio arrives. A faster
  FFT buys CPU headroom *during* the slot, which is B54's concern. The guard-band window is spent in
  `ftx_find_candidates()` and the per-candidate LDPC, which is also where OSD and subtraction would land.
- **The P4 build has PSRAM off** (`# CONFIG_SPIRAM is not set` in `poe_p4/sdkconfig`). Its linker map offers
  `sram_low` 0x2cbd0 + `sram_high` 0x60000, about **567 KB of internal RAM**, plus an 8 KB tightly-coupled
  `spm_idram_seg`. The t2/f2 waterfall is 161,076 B and **fits in internal RAM**. So the P4's first tuning
  round may never touch PSRAM, and §3's PSRAM-latency concern applies only to configurations larger than
  that. That is a budget to check against everything else resident on the P4, not a free 567 KB.

**Host indication for the compiler lever** (Apple M4, 24 corpus files, decode counts identical):

| Build | `-Og` | `-O2` | Speedup |
|---|---|---|---|
| kgoba `ft8_lib` | 12.0 ms/slot | 9.8 ms/slot | 1.2× |
| `howard0su/ft8_lib` | 116 ms/slot | 103 ms/slot | 1.1× |

A wide out-of-order desktop core hides much of what `-Og` leaves behind. The S3 and P4 are in-order cores,
where the gap is usually larger. How much larger is a device measurement.

**Levers, ranked by expected gain per unit of effort.** Each is an A/B on [B87](../ROADMAP.md)'s device
benchmark (§5 P0), on both boards:

1. **`-O2` for the decoder only.** A `target_compile_options` line for `ft8_lib` and `decode_pipeline`; the
   rest of the image stays at `-Og` and debuggable. Output must be identical, so the benchmark's
   decode-set check is the whole test.
2. **The second core.** Both chips are dual-core and decode runs on one. Per-candidate LDPC is independent
   per candidate, so the loop splits across cores naturally. That could approach 2× on exactly the window
   that decides how much of §4.1's feature list fits. The constraint is what already runs on the other core (the
   UI on the ADV, USB host on both). Measure that core's load before assuming it is free.
3. **Placement.** Code runs from flash through a cache on both chips, and an in-order core pays for every
   miss. `IRAM_ATTR` on the candidate search and LDPC loops, and on the P4 the waterfall in internal RAM
   (above), and possibly the hottest loop in the 8 KB TCM.
4. **`esp-dsp` FFT in place of `kiss_fft`.** Capture-time CPU headroom for B54, not decode time.
5. **SIMD, last. Both chips' vector units are integer-only (confirmed 2026-09-23).** Two independent checks:
   - The ESP-IDF toolchain's own assemblers (esp-14.2.0) accept integer vector ops and reject float ones.
     On the P4 (`riscv32-esp-elf-as -march=rv32imafc_xesppie`), `esp.vmul.s16`/`.s8`/`.u8`,
     `esp.vadd.s32`, `esp.vcmp.gt.s16` and `esp.vmax.s16` assemble, and `esp.vmul.f32`/`esp.vadd.f32` do
     not. On the S3 (`xtensa-esp32s3-elf-as`), `ee.vmul.s16`/`.s8`, `ee.vadds.s32` and `ee.vmax.s16`
     assemble, and `ee.vmul.f32`/`ee.vadds.f32` do not. A rejected guessed name alone would prove little,
     which is why the second check matters.
   - Espressif's own P4-optimised float code in `esp-dsp` (`dsps_dotprod_f32_arp4.S`,
     `dsps_fft2r_fc32_arp4.S`) uses no vector instructions at all: scalar `flw`/`fmadd.s`/`fsw` plus the P4's
     zero-overhead hardware loop, `esp.lp.setup`. If the vendor had float SIMD, its float DSP library would
     use it.

   So SIMD suits the candidate search, which sums over a `uint8` waterfall, and not the `float` LDPC.
   Vectorising LDPC means a fixed-point normalised min-sum decoder. That is an algorithm change with a usual
   small sensitivity cost, so it gets scored on the P1 harness like any other. It is per-chip intrinsics or
   assembly work, so it comes only after levers 1–3 are measured. The P4's hardware loop is a separate,
   smaller lever that applies to `float` code too. Whether GCC emits it on its own at `-O2` is unchecked;
   `esp-dsp` (lever 4) uses it by hand. Disassembling P0's `-O2` image for `esp.lp.setup` answers that.

**No help:** the P4's media and crypto accelerators (pixel processing, JPEG, H.264, AES/SHA). Neither chip
has an FFT or DSP accelerator block.

**Bearing on the ADV question (§8):** levers 1–3 apply to the S3 as well. §4.1's cheap features
(candidates, subtraction, passes) need CPU and no memory, so these levers decide how many of them the S3 can
afford inside its guard band.

## 5. Phases

Each phase is a slice with its own done-when. P0 and P1 are the candidates for Now. P2 is blocked on both;
the others are independent of each other unless stated.

### P0 — Device decode benchmark ([B87](../ROADMAP.md))

First, because four questions wait on it (§3): the real desktop-to-device factor, host/device parity, ADV
against P4, and §4.2's levers. It does not depend on P1 and is small.

A `test_apps/` image per board, sharing one source, that embeds a few corpus slots (resampled to 6 kHz on the
host, stored in flash) and decodes each under a list of configurations. It prints, per slot and
configuration, the decoded messages and the wall time of candidate search plus decode. Configurations cover
§4's parameters and §4.2's levers 1–3. `howard0su/ft8_lib` joins when P5 starts.

**Tiering:** an agent builds both images and checks the host side. Flashing and reading the serial output
are the operator's. USB-C is free of the radio for this, so the console works over USB on both boards.

**Done-when:** a table in this RFC of milliseconds per slot, per configuration, per board, against the
2.36 s guard; each board's decode sets compared with the host's for the same files; and §4.1's 30–50×
estimate replaced by the measured factor.

### P1 — Host harness on the existing corpora ([B25](../ROADMAP.md))

`tests/tx_e2e`'s `decode_pcm()` (`tests/tx_e2e/decode_helper.cpp`) already runs `monitor_process` →
`ftx_find_candidates` → `ftx_decode_candidate` over WAVs in CI. So this is a generalisation, not a new
build. As it stands it cannot falsify a single parameter:

- it hardcodes its own values (`max_cand` 20, `min_score` 0, LDPC iterations 4, literal osr values) instead
  of the firmware's;
- it `break`s on the first decode, so it cannot count yield;
- each WAV is one clean synthesised signal;
- it skips the firmware's per-block AGC.

Scope:

1. A host CLI that runs the firmware's decode with every §4 parameter as a flag, including the per-block
   AGC in `ft8_audio_pipeline_run()` (on/off) and the waterfall's quantisation (levels per cell, for §4's
   fewer-bits experiment). It prints one line per decode in `jt9`'s format.
2. Truth regeneration: pinned `jt9` over the corpus, with version and depth recorded.
3. A scorer: recall, SNR-bucketed recall, extras, discordant counts between two runs.
4. Synthetic tier 1: GFSK at calibrated SNR, for the absolute curve.

**Two corpora:** the in-tree set and `howard0su/ft8_lib`'s `test/wav/20m_busy` (38 busy 20 m slots), both
with truth regenerated by pinned `jt9`. The busy set is the one that exercises the candidate cap.

The corpus WAVs are 12 kHz. The ADV decodes at 6 kHz, and P3 recommends 12 kHz for the P4. For 6 kHz
configurations the harness decimates 12 → 6 with a proper filter, **not** the firmware's stride decimator;
12 kHz configurations take the WAVs unchanged. That keeps P1 answering decoder questions only; the
decimator is P3's question.

**Done-when:** one command produces the §2 report for a named configuration against the corpus and the
synthetic set, and a CI job runs it on today's settings as a regression baseline.

### P2 — Round 1: ADV configuration against P4 configurations ([B25](../ROADMAP.md))

A small number of large moves, not a grid search. With two small corpora there is no honest held-out set,
and a grid over hundreds of points would find a chance winner. Every configuration is scored on both corpora
(§4.1's disagreement over the swap is the reason).

- today (t2/f1, 50 candidates, `DEC_MAX` 32);
- **candidates 256, both boards.** §4.1 shows the 50 cap losing ~5 points on busy slots for CPU only. This
  is the first change to ship, and it needs no memory;
- **subtraction (fixed, fraction tuned) + 3 passes, both boards.** No memory;
- the S3-reachable swap (t1/f2). §4.1's two corpora disagree on it (+2.8 and −0.2 points), so it stays in;
- **S3, equal memory: t2/f2 with values quantised to 16 levels** (simulated, one value per byte; §4's
  fewer-bits section), scored against 8-bit t2/f2. §4.1 puts `freq_osr` 2 at +10 points, so this is the
  ADV's deciding experiment. If the gap is within the discordant-count noise, packing is worth building;
- P4: t2/f2; t4/f2; `DEC_MAX` 50; the combination.

**Caveat:** the in-tree corpus is the one kgoba developed `ft8_lib` against, and `20m_busy` is the one
howard0su tuned his fork on. Each favours its author's settings. A win for them is expected, not proof.

**Done-when:** a table in this RFC with recall, SNR-bucketed recall, and discordant counts per
configuration; P0's device benchmark run on both boards for every configuration, against the guard band;
and the host/device parity check passing on each board.

### P3 — Decimation and the 48 kHz question ([B85](../ROADMAP.md))

`resample_48k_to_6k()` in `components/resample/resample.cpp` keeps every eighth sample with no anti-alias
filter. The comment justifies this as "input is already bandwidth-limited". If the QMX's USB audio carries
anything above 3 kHz, it folds into the passband. The noise-floor penalty is
10·log10(1 + P(3–24 kHz) / P(0.2–2.9 kHz)): about 0 dB if the QMX filters at its edge, up to about 9 dB if
its noise is flat to 24 kHz. WSJT-X filters before decimating.

1. **Measure first, no firmware change:** record 15 s of the QMX at 48 kHz on a Mac and compute that ratio.
   The same recording also shows whether left and right carry identical audio, which
   `uac_pcm_to_ft8_samples()` assumes when it averages them.
2. If the penalty exceeds ~1 dB, build a two-stage decimator (48 → 12 → 6 kHz, passband to 2.9 kHz,
   stopband by ~3.1 kHz). Estimated cost is 1–2 M multiply-adds per second and a few hundred floats of state,
   which fits both boards. A host tone-sweep test proves the stopband.

**The sample rate is not the lever; the missing filter is.** FT8 lives in 200–2900 Hz (`f_min`/`f_max` in
`ft8_audio_pipeline.cpp`), and any rate of at least ~6 kHz carries that band completely. The waterfall's size
does not depend on the rate either: it holds only the bins between `f_min` and `f_max`, 6.25 Hz apart (at
`freq_osr` 1), whatever the rate. What the rate changes is the FFT per block (`nfft` = rate × 0.16 s ×
`freq_osr`), so a higher rate costs capture-time CPU and FFT scratch memory, not decode quality. Unfiltered
decimation is what costs decode quality, in two ways:

- **Noise.** Everything in 3–24 kHz folds onto the passband, per the formula above.
- **Phantom signals.** A real signal anywhere in 3–24 kHz lands at a mirrored frequency inside the passband,
  where the decoder can find it at the wrong frequency, or where it masks a weak real one.

**What the QMX actually sends** (QMX operating manual, firmware 1_00_024, §5.22's receive signal path and
§8.6; the manual describes it, the design rationale below is ours):

1. The QSD mixes the band to a **12 kHz low IF**, not to 0 Hz. The PCM1804 ADC samples I and Q at 48 ksps, and
   the DSP mixes the 12 kHz IF down digitally ("The SDR implements a superhet with 12kHz IF", image 24 kHz
   down). Keeping the wanted signal away from 0 Hz is the standard way to dodge DC offset, 1/f noise and LO
   leakage in a direct-conversion front end. That is the frequency-offset trick in this design.
2. I and Q are **decimated to 12 ksps**, the Hilbert transform and sideband sum run there, and in Digi mode the
   audio passes the **"default 150-3200Hz wide filter used for Digital modes"**.
3. The result is **interpolated back to 48 ksps** ("including implicit anti-alias filter") for the USB sound
   card. **The QMX's AGC does not touch it**: "The AGC system is only operational in CW mode."

So the USB audio carries no more information than a 12 ksps stream with a 3.2 kHz passband. Consequences:

- **Everything above 6 kHz is interpolation residue**, not band noise. Its images sit at 12 kHz ± f, and 12 kHz
  is a multiple of our 6 kHz, so any residue folds back onto the *same* frequency it came from. It cannot land
  somewhere new. The broadband-noise worst case above (~9 dB) does not apply to a QMX.
- **Keeping every 8th sample of the 48 ksps stream is, in effect, keeping every 2nd sample of the QMX's
  12 ksps stream.** So what folds is the QMX's 3–6 kHz content, and the QMX's own passband runs to 3.2 kHz.
  **Its 3.0–3.2 kHz band folds, at full level, onto 2.8–3.0 kHz**: a signal the QMX passes at 3,150 Hz appears
  in our decode window at 2,850 Hz (checked numerically 2026-09-23 with a generic ×4 interpolator; tones below
  3 kHz are untouched). Over 2.8–2.9 kHz that doubles the noise (+3 dB) and adds mirrored phantom signals. The
  3.2–6 kHz skirt of the QMX filter folds onto the rest of the window at whatever that skirt's attenuation is,
  which the manual does not give.
- **So for a QMX, the missing filter costs the top ~100 Hz of the window, not the whole band.** It is a real
  defect (phantoms are worse than noise, because they decode as real stations at the wrong frequency), but it is
  not the large lever the generic analysis allowed for. The stand-in in §4.1 decodes WAVs that never went
  through our decimator, so it cannot see even this.
- **The comment "input is already bandwidth-limited" is nearly true for a QMX, to 3.2 kHz rather than 3.0.**
  Other radios, especially ones with an analog audio path into a codec, can be far worse. The fix should not
  depend on which radio is attached.
- **L/R and IQ mode.** The manual does not say what normal mode puts on each USB channel.
  `uac_pcm_to_ft8_samples()`'s L/R average assumes both carry the same demodulated audio, and the Mac recording
  checks that. With the QMX's **IQ mode** enabled, the channels are
  raw I and Q at the 12 kHz IF; averaging them yields no usable audio, and decode would fail silently. The
  manual says IQ mode "is not suitable for use with WSJT-X and other Digi mode programs". A cheap L/R
  correlation check could turn that into a visible warning.
- **The P4's 12 kHz choice below lines up with the radio.** Filtered 48 → 12 kHz decimation is the inverse of the
  QMX's own interpolation and recovers its 12 ksps stream.

The measurement in step 1 still stands: it confirms the 3.2 kHz passband and gives the skirt's level. The QMX's
built-in "Audio filter sweep" (manual §8.6.1, 100–3,850 Hz) shows the start of that skirt without a Mac, but
stops short of 6 kHz.

**On the P4 there are three reasonable choices**, and all three fix the aliasing:

| Option | Filter | FFT per block | Upside | Downside |
|---|---|---|---|---|
| 48 → 6 kHz, filtered | two-stage FIR, as above | `nfft` 960 × `freq_osr` | one code path for both boards | none on quality |
| **48 → 12 kHz, filtered** | 4:1 FIR, simpler than 8:1 | `nfft` 1920 × `freq_osr` | matches WSJT-X and every corpus WAV, so P1 feeds WAVs unchanged and host/device parity has one step fewer; passband can reach ~5 kHz if the radio passes it | ~2× the capture-time FFT work and scratch; trivial on the P4 |
| no decimation, FFT at 48 kHz | none: discarding out-of-band bins *is* the filter, up to window sidelobes | `nfft` 7680 × `freq_osr` | no filter code at all | ~8× the FFT work and a much larger FFT arena, to get what a cheap FIR gives |

**Recommendation: 48 → 12 kHz on the P4**, filtered, and 48 → 6 kHz filtered on the ADV, where the FFT
scratch memory matters. The quality difference between 6 and 12 kHz, once both are filtered, is nil below
2.9 kHz. 12 kHz is chosen for parity with the corpus and WSJT-X, not for sensitivity. P0 times the FFT at
both rates on the P4 before this is final.

**Done-when:** the measured dB figure is recorded here. Then either the comment is corrected and the
question is closed, or the filtered decimator ships on both boards (12 kHz on the P4) with its host
tone-sweep test and a field A/B.

### P4 — Capture-path health on hardware ([B53](../ROADMAP.md), then [B54](../ROADMAP.md))

Field-only. B53 is instrumentation; B54 fixes what it finds.

**B53, partly built.** `decode_monitor_results()` already logs `SLOT cand=… msg=… dec=…ms` per slot
(`cf1bbc3`). Two numbers are missing:

- **`g=`, the surviving guard blocks at the slot boundary.** `slot_blocks` resets at the slot boundary and
  again after a decode, so the count at a boundary is the guard band that survived decode. About 14 is
  healthy (93 per slot minus 79 consumed). About 0 means decode ate the guard. Fewer than 79 with no decode
  at all means the audio never arrived.
- **`lost=`, the sample-arrival deficit.** Accumulate `cfg->read`'s return against 90,000 samples per 15 s
  slot.

Do **not** read these by raising `CONFIG_LOG_MAXIMUM_LEVEL`. That compiles in every `ESP_LOGD` in the tree,
and the resulting flood is its own starvation source. FT4 is the sharper test: a near-identical guard band
but 105 symbols to decode, and a 16 KB decode stack against FT8's 8 KB. `cand=` also answers whether a busy
band ever reaches 50 candidates. If it never does, raising candidates buys nothing on real bands, whatever
P2 says.

**B54, two coupled defects, desk-audited 2026-09-12 and unmeasured.**

- **(a) No app-side buffer between USB and the decoder.** `uac_read_ft8_samples()` reads the driver
  directly. Total queue depth is `UAC_BUFFER_SIZE` 4000 B = 13.9 ms at 48 kHz / 24-bit / stereo. On
  ring-full, the vendored driver (`managed_components/espressif__usb_host_uac/uac_host.c`) drops the
  transfer with an `ESP_LOGD` our build compiles out. Its `RX_DONE` event is never consumed and fires
  constantly anyway. There is no countable overflow signal, which is why B53 measures the deficit app-side.
- **(b) Single-buffered capture, decode in the capture task.** `ft8_audio_pipeline_run()` decodes inline.
  Whatever decode takes is audio dropped at the wire. Decode on our own TX slot is skipped deliberately, and
  that is not this defect.

A constraint on the obvious fix: `uac_read_ft8_samples()` ignores `max_samples` and writes into a
512-float temp buffer. At 6 B/frame and 8:1 decimation, the implicit ceiling is
`UAC_READ_BUFFER_SIZE` ≤ 24,576 B, and growing past it overruns silently.

**Done-when:** B53's line from a busy-band field run, FT8 and FT4, reads healthy (`lost`≈0, `g`≈14, `dec`
well under the guard) or sick (`lost` tracking `dec`, `g` collapsing on high-`cand` slots). If sick, fix
with an app-side ring deep enough to cover decode, then move decode off the capture task — one at a time,
measured per `TEST_PLAN.md`'s decode-yield protocol.

### P5 — Algorithm changes ([I26](../ROADMAP.md))

Per §4.1, this is a feature list taken from `howard0su/ft8_lib`, not a library swap. Candidates, subtraction
and passes land in P2 because they are cheap on both boards. What remains, in order:

1. **Better subtraction.** The fork lowers only the decoded tone's bin at one `time_sub`. Clearing the same
   signal's energy in neighbouring `time_sub` positions is the obvious next step, scored on the harness.
2. **`time_osr` 4 on the P4**, if P0 says the CPU fits.
3. **AP decoding written from the QEX paper**, scored separately, because the corpora have no QSO context and
   AP's gain shows only when the decoder is told what to expect. That needs its own test: synthetic QSO
   exchanges at falling SNR with the partner's call supplied. **Restrict QSO-context AP by frequency, as WSJT-X
   does.** `lib/ft8/ft8b.f90` skips AP types 3–6 ("MyCall DxCall ???", RRR, 73, RR73; 61–77 of 77 bits
   pre-filled) for any candidate more than `napwid` from both its Rx (`nfqso`) and Tx (`nftx`) frequency.
   `widgets/mainwindow.cpp` sets `napwid` to 50 Hz for FT8, and its multithreaded FT8 decoder narrows it to 5 Hz
   below 30 MHz (checked 2026-09-23 against the `WSJTX/wsjtx` GitHub mirror). Types 1–2 ("CQ ???", "MyCall
   ???") run everywhere. With most of a message pre-filled, a decoder searching the whole band can turn noise
   into a plausible "RR73", so ours applies strong AP only within a few Hz of the partner's known offset. The
   synthetic test scores false decodes as well as recall for that reason.
4. **OSD, last and optional**, only bit-packed (not the fork's ~40 KB of byte-per-bit stack), and only if
   its ~1.3 points survive P0's timing and its extra false decodes are acceptable.

**Done-when:** per change, a discordant-count result on the corpus, device timing on the P4, and a decision
(ship on P4 / ship on both / drop) recorded here.

### P6 — Corpus at scale ([B86](../ROADMAP.md))

What the in-tree corpus cannot provide: our own audio chain at 48 kHz, weak signals near threshold, very
crowded slots, and enough files for a held-out set.

- **Our chain:** the QMX on a Mac, recorded at 48 kHz in slot-aligned files, tapped at the USB wire. macOS
  lets WSJT-X run live on the same device alongside the recording.
- **Volume:** kiwirecorder (`jks-prv/kiwiclient`, `--dt-sec 15` for slot-aligned files) against public
  KiwiSDRs, many bands and locations, unattended. It is someone else's receiver, so it tests the decoder,
  not our audio path.
- **Storage:** 48 kHz 16-bit mono is ~1.4 MB per slot, ~345 MB per hour — not git. A checked-in manifest
  (file list, SHA-256, truth provenance) plus a download location, so a fresh clone can rebuild the corpus
  per `docs/README.md`'s reproducibility rule. Location undecided.
- **Consensus truth:** add a second independent decoder (JTDX is the obvious one) to reach tier 5.

**Done-when:** a corpus large enough to hold out a test set, and P2's winner re-scored against it.

## 6. Non-goals

- **A turnkey benchmarking tool.** Existing pieces are used as-is (`jt9`, kiwirecorder, kgoba's corpus);
  we write only the glue. FT8Harvest (WSJT-X save folder → dataset) was considered and is too small to be
  worth a dependency.
- **Chasing `freq_osr` = 2 on the S3.** §4 proves it does not fit. The reachable S3 experiment is the swap.
- **An optimiser.** The parameter space is small enough to enumerate, and the corpus is too small to
  support searching it finely.
- **Porting code from `qmx-panadapter`** without the operator's own read of its license and code.

## 7. Prior art

| Source | What we take from it |
|---|---|
| `jt9` (WSJT-X 3.0.1, `/usr/local/bin/jt9` on the operator's Mac) | Reference decoder and truth source |
| `kgoba/ft8_lib` `test/wav/` + `utils/run_tests.py` | First corpus and scorer |
| pancetta `docs/decoder-comparison.md` | Method: recall-vs-SNR against `jt9`, false-decode rate on pure noise |
| Web-888 / rx-888.com "Optimizing the FT8 Skimmer Experience" | Recall/precision/F1 against WSJT-X; OSD, subtract, multi-pass |
| `howard0su/ft8_lib` | Likely source of the above; dissected in §4.1; source of the feature list and of the `20m_busy` corpus |
| Franke, Somerville, Taylor, "The FT4 and FT8 Communication Protocols", QEX 2020 | Algorithm source for AP decoding without GPL code |
| QRP Labs, QMX operating manual, firmware 1_00_024 (§5.22 receive path, §8.6 hardware tests) | What the QMX's USB audio contains: 12 kHz IF, 12 ksps DSP, 150–3,200 Hz Digi filter, interpolation to 48 ksps, AGC in CW only (P3) |
| Hans Summers G0UPL, "Bringing SSB to QMX" (FDIM) | "the receiver IS an SSB receiver, in Digi modes"; receiver anatomy ending in a sharp IIR audio filter |
| `jks-prv/kiwiclient` kiwirecorder | Slot-aligned recording from public SDRs |
| `e04/ft8ts` benchmark | Independent evidence of the `ft8_lib`–WSJT-X gap |

## 8. Open

- **Whether the ADV stays supported.** The operator has said (2026-09-23) they would drop ADV support if it
  becomes a heavy tax to maintain. This RFC is where that tax shows up first: two decoders, two memory
  budgets, the S3-only experiments in P2, two sample rates (P3), and a device benchmark per board. §4.1 lowered
  it: the recommendation is now one decoder with features switched per board, not two libraries. If the ADV
  is dropped, P2 loses its ADV arm and §4's S3 analysis becomes history. Nothing here should be built *only*
  to keep the ADV viable until that is decided. The P2 16-level experiment bears directly on it: `freq_osr` 2
  is worth ~10 points (§4.1), and if 4-bit t2/f2 holds, the S3's gap to the P4 is mostly CPU, not memory.
  [RFC 0005](0005-p4-poe-headless-port.md)'s "both targets build from every build" (its §7 Phase 0) is the rule
  it would change.
- Corpus storage location (§5 P6).
- Whether the ADV should adopt anything from P2 beyond the S3-reachable swap and `DEC_MAX`.
- Whether `DEC_MAX` above 32 needs an RX-list UI change on the ADV, or only a larger internal buffer.
