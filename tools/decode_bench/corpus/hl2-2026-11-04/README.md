# HL2 band survey, 2026-11-04

The Hermes-Lite 2 recorded on a computer, one band at a time, 30 minutes a band, and scored with our decoder against `jt9` on the same samples: what row mg9c (HL2 receive, one band) asked for, and the HL2 counterpart of the [QMX corpus](../qmx-2026-11-02/README.md). The audio is not in git; it goes up as a GitHub release when the run is complete, one tar per band. The manifests here are copies, so every file's SHA-256 is in git.

## The station and the run

* HL2, gateware 74.2, with the N2ADR filter board; the same 40 m end-fed half-wave as the QMX corpus, good on 40, 20, 15 and 10 m (its harmonics) and poor on the others; receive only.
* Its Ethernet straight into a Linux laptop's port with a link-local address; the laptop's clock NTP-synced (0.3 ms from NTP during the run, 40 ms RMS).
* `tools/hl2_record.py`, a plan of ten bands in turn, 120 slots each, the night bands waiting for dark; the HL2 driven as the P4 drives it: one receiver at 48 kHz, its NCO on the dial, the band's N2ADR filters, the LNA from +20 dB stepping down while the ADC clips. Started 20:21:45 UTC.
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

**Our share of `jt9` is the QMX's.** On the QMX corpus's 15 m I/Q (2026-11-02) ours was 16.6 of `jt9`'s 24.4 a slot, 68%; here 64–75% on every band up to about 22 `jt9` decodes a slot. 20 m at 50 a slot drops to 48%, the crowded-band plateau the QMX corpus showed too (our decoder levels off near 25–27 a slot whatever `jt9` finds; [decode.md](../../../../docs/architecture/decode.md), **Subtraction**). So the HL2's receive chain costs our decoder nothing this measures; the gap is the decoder's, which rows kkap (subtraction from the audio) and qauk (AP decoding) work on. The decode counts themselves measure the band, not the radio: comparing the two radios needs both on one antenna at once (epic [knpp](../../../../docs/roadmap/hl2-support.md), **Later**).

**The LNA ends every band at +14 dB or lower, from a +20 dB start** (20 m at +11). Each stream's first second at +20 dB read 33 to 61% of its status words clipped, and one later burst stepped it again (epic, **Open**).

**The image check does not apply.** `score_ab.py` swaps I and Q to find each strong station's mirror, which on a QMX is its image across the LO; on the HL2, a direct-sampling radio, the far side of the NCO is real spectrum below the dial, so its "wrong-side" decodes (3 on 15 m, 9 on 20 m) are other stations, not images.

**Timing:** each slot's first packet arrived within −1 to +2 ms of its UTC boundary over each band, so the HL2's sample clock is within about 1 ppm of the laptop's (the QMX's ran about 150 ppm fast); `jt9`'s DT median on 15 m was 0.1 s.

The night bands (40, 60, 80 and 160 m) are added here as they are scored.
