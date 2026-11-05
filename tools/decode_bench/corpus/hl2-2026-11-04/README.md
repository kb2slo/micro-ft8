# HL2 band survey, 2026-11-04

The Hermes-Lite 2 recorded on a computer, one band at a time, 30 minutes a band, and scored with our decoder against `jt9` on the same samples: what row mg9c (HL2 receive, one band) asked for, and the HL2 counterpart of the [QMX corpus](../qmx-2026-11-02/README.md). The audio is not in git. It is the release [`corpus-2026-11-04-hl2`](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-04-hl2), one tar per band holding that band's WAVs and its `manifest.jsonl`, and the run's log. The manifests here are copies, so every file's SHA-256 is in git.

| File | SHA-256 |
| --- | --- |
| `hl2-survey-2026-11-04-10m.tar` | `4d13d88037ec4de6e432d07790b1a52a819ef6353d5719fec45ae27f87795fba` |
| `hl2-survey-2026-11-04-12m.tar` | `383f073e524ea1903bc3cb6c61f9182a91f261e2ff7dfb883ee9fcbfaf55fed7` |
| `hl2-survey-2026-11-04-15m.tar` | `12d3d076a381f06e85bd83282293da9b29c55b9000679602b77ba3cff3bc2dbd` |
| `hl2-survey-2026-11-04-17m.tar` | `a48704e45fac48a82c0a13c00f040c59360f9ffc361180d0b9ed36e80b4118b4` |
| `hl2-survey-2026-11-04-20m.tar` | `287cd460e2aed778aa5e407e0f73562f602b8fc0566cf93c76e4f0a4153a419b` |
| `hl2-survey-2026-11-04-30m.tar` | `009ee7f495c789c0ca642e1ee3870b16c13945066d1ed1f96640d8f6b8109f37` |
| `hl2-survey-2026-11-04-40m.tar` | `28aa44aa91108f589830b8330df8c1660b0491ff08009630e7dbb4c3ba4d112d` |
| `hl2-survey-2026-11-04-60m.tar` | `3adc398966f0f8f9ecc8e53d01a5ef08b0f50c9864a0a974c3cfc16dcab5c3c6` |
| `hl2-survey-2026-11-04-80m.tar` | `b940cb97633029f8085869a791d80387429223c3f66af6bab6b174a22a483d2d` |
| `hl2-survey-2026-11-04-160m.tar` | `2d91e0ba42a3b367082fbde72c84b990a426f6a92f0123ffd39bcd75a570562b` |
| `hl2-survey-2026-11-04-run.log` | `cfd94cb2c970b802a593070f82784833e34ba5e482969a8a603470cfec562a1b` |

## The station and the run

* HL2, gateware 74.2, with the N2ADR filter board; the same 40 m end-fed half-wave as the QMX corpus, good on 40, 20, 15 and 10 m (its harmonics) and poor on the others; receive only.
* Its Ethernet straight into a Linux laptop's port with a link-local address; the laptop's clock NTP-synced (0.3 ms from NTP during the run, 40 ms RMS).
* `tools/hl2_record.py`, a plan of ten bands in turn, 120 slots each, the night bands waiting for dark, 20:21 to 02:30 UTC, 1,200 slots, 4.9 GB; the HL2 driven as the P4 drives it: one receiver at 48 kHz, its NCO on the dial, the band's N2ADR filters, the LNA from +20 dB stepping down while the ADC clips. Started 20:21:45 UTC.
* **Not the P4's figures.** Packets lost and late here describe the laptop's receive, not the P4's network stack, whose own counters are in its log ([main_board.md](../../../../docs/architecture/main_board.md), **Ethernet**). A direct cable does not reorder packets, so late packets are 0 on either.

## How it was scored

`python3 tools/decode_bench/score_ab.py BAND_DIR --center-hz 1500 --iq-flag=--negate-q --jobs 2`: each slot through `tools/iq_convert` at the HL2's channel (1,500 Hz above the NCO, Q negated), then `jt9 -8 -d 3` and `tools/decode_recall` at the firmware's settings and at t4/f2.

## Results

| Band | Antenna | `jt9` per slot | Ours per slot (share of `jt9`) | t4/f2 | `jt9` SNR median | Lost / late packets | Clipped status words | LNA | HL2 °C |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10 m | good | 5.7 | 3.7 (64.4%) | 3.9 (68.3%) | −14 dB | 0 / 0 | 26 of 342,858 | +17 → +14 dB | 26.3–28.8 |
| 12 m | poor | 3.1 | 2.3 (74.9%) | 2.4 (76.8%) | −10 dB | 0 / 0 | 19 | +17 → +14 dB | 27.9–28.8 |
| 15 m | good | 22.2 | 14.8 (65.4%) | 15.3 (67.7%) | −7 dB | 0 / 0 | 17 | +17 → +14 dB | 28.0–29.0 |
| 17 m | poor | 17.6 | 11.3 (64.0%) | 12.0 (67.6%) | −10 dB | 0 / 0 | 26 | +17 → +14 dB | 28.2–29.2 |
| 20 m | good | 50.0 | 24.3 (48.4%) | 25.0 (49.8%) | −2 dB | 0 / 0 | 13 | +14 → +11 dB | 28.0–29.0 |
| 30 m | poor | 18.1 | 12.8 (70.6%) | 13.2 (72.9%) | −5 dB | 0 / 0 | 26 | +14 → +14 dB | 29.4–30.1 |
| 40 m | good | 44.5 | 22.1 (49.5%) | 22.9 (51.2%) | −4 dB | 0 / 0 | 13 | +11 → +5 dB | 28.5–29.4 |
| 60 m | poor | 2.0 | 1.4 (71.9%) | 1.4 (73.6%) | −10 dB | 0 / 0 | 11 | +11 → +8 dB | 28.3–29.4 |
| 80 m | poor | 11.0 | 6.8 (61.5%) | 7.1 (64.9%) | −12 dB | 0 / 0 | 2 | +8 → +8 dB | 28.2–28.9 |
| 160 m | poor | 3.2 | 2.4 (75.1%) | 2.5 (76.4%) | −10 dB | 0 / 0 | 16 | +20 → +20 dB | 28.6–29.6 |

**Our share of `jt9` is the QMX's.** On the QMX corpus's 15 m I/Q (2026-11-02) ours was 16.6 of `jt9`'s 24.4 a slot, 68%; here 62–75% on every band up to about 22 `jt9` decodes a slot (160 m 75%). 20 m at 50 a slot drops to 48%, and 40 m after dark at 44.5 to 50%, the crowded-band plateau the QMX corpus showed too (our decoder levels off near 25–27 a slot whatever `jt9` finds; [decode.md](../../../../docs/architecture/decode.md), **Subtraction**). So the HL2's receive chain costs our decoder nothing this measures; the gap is the decoder's, which rows kkap (subtraction from the audio) and qauk (AP decoding) work on. The decode counts themselves measure the band, not the radio: comparing the two radios needs both on one antenna at once (epic [knpp](../../../../docs/roadmap/hl2-support.md), **Later**).

**The LNA ends every band but 160 m at +14 dB or lower, from a +20 dB start** (20 m at +11; 40 m after dark at +5, 15 dB down, with its share of `jt9` on the same plateau as 20 m's, so the steps are not what limits it there). **160 m never stepped:** its first second did not clip, though it is the one band whose filter relays bypass the receive high-pass. So the first-second clipping on the other nine is probably not the relays switching but strong signals that 160 m's low-pass removes (everything above about 2.5 MHz, the broadcast bands among them). Each stream's first second at +20 dB read 33 to 61% of its status words clipped, and one later burst stepped it again (epic, **Open**).

**The image check does not apply.** `score_ab.py` swaps I and Q to find each strong station's mirror, which on a QMX is its image across the LO; on the HL2, a direct-sampling radio, the far side of the NCO is real spectrum below the dial, so its "wrong-side" decodes (3 on 15 m, 9 on 20 m) are other stations, not images.

**Timing:** each slot's first packet arrived within −1 to +2 ms of its UTC boundary over each band, so the HL2's sample clock is within about 1 ppm of the laptop's (the QMX's ran about 150 ppm fast); `jt9`'s DT median on 15 m was 0.1 s.

**Which recorder ran.** One process recorded every band, with the code it loaded at 20:21 UTC: `tools/hl2_record.py` as it stood then, uncommitted on top of `1855ad7`, one receiver, the band plan and the failsafes; the four-receiver rewrite was committed (`57ee4e3`) after it started and did not run here. Each manifest's `recorder` is `git describe` of the worktree at each band's start, so it reads `1855ad7-dirty` (10–15 m), `62b3dd2` (17–20 m), `5119b3a-dirty` (30 m) and, once that worktree was removed, `unknown` (40 m on): the checkout's state, not the running code.

**Not in the manifests:** the N2ADR filter byte. The code that ran predates that field; each band's byte follows from its dial (`hl2_n2adr_receive_filters()`), and `run.log` shows it.
