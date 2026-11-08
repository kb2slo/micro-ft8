# QMX IQ-versus-audio corpus, 2026-11-02

Stage 2 of row y9bt (decode from the QMX's IQ mode): the same QMX recorded in IQ mode and in its
normal audio mode, two slots of each in turn, on three bands. The audio is not in git. It is the
release [`corpus-2026-11-02`](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-02),
one tar per band holding that band's WAVs and its `manifest.jsonl`. The manifests here are copies,
so every file's SHA-256 is in git.

| Tar | SHA-256 |
| --- | --- |
| `qmx-iq-ab-2026-11-02-15m.tar` | `221d32e348fb75a83f035d5b15c0cb4f5de06f254311c1a97a8e16b43f2070d2` |
| `qmx-iq-ab-2026-11-02-20m.tar` | `252682b962cbb157aa74954915437313af9021c6339b8fac5827761e9f61ea2d` |
| `qmx-iq-ab-2026-11-02-40m.tar` | `b96f5d0f1df77ba8541b39b9a326582a88862711b0acb801ae090178bcf1a426` |

## The station and the run

* QMX, firmware 1_04_010, a 40 m end-fed half-wave tuned for 40/20/15 m, the QMX's USB-C straight
  into a Linux workstation with NTP-synced time.
* `tools/qmx_record.py --alternate 2 --minutes 30`, starting in IQ mode: 120 slots per band, 60
  each mode, 48 kHz stereo 24-bit, 0 overflows, every slot's `Q9;` reply matching its mode.
* 15 m 20:30:45–21:00:30 UTC at 21.074 MHz; 20 m 21:02:45–21:32:30 at 14.074; 40 m 21:33:15–22:03:00
  at 7.074. The manifests' `recorder` field reads `92bbb47-dirty` for 15 m and 20 m: that was the
  `--alternate` change before it was committed as `ef4b48a`.
* The QMX's sample clock ran about 150 ppm fast against UTC, so by a run's last slot its file starts
  about 275 ms early (`first_frame_arrived_utc`).

## How it was scored

With `tools/decode_bench/score_ab.py` (its defaults): IQ files through `tools/iq_convert` (`--center-hz 13500`, I on the left channel, Q not negated, no
IF trim, `--normalize-db -20`); audio files with `--real --normalize-db -20`. Both at 12 kHz to
`jt9 -8 -d 3` and to `tools/decode_recall/decode_recall` at the firmware's settings. "Paired" sets
each two-slot IQ turn against the audio turn after it.

| Band | jt9 per slot, IQ / audio | Ours per slot, IQ / audio | Paired IQ − audio per turn, jt9 | Image rejection (median, wrong-side decodes) |
| --- | --- | --- | --- | --- |
| 15 m | 24.4 / 25.5 | 16.6 / 17.1 | −2.23 ± 0.90 | 44 dB (1) |
| 20 m | 56.2 / 55.9 | 26.8 / 26.6 | +0.53 ± 1.47 | 32 dB (173; range 20–50) |
| 40 m | 10.1 / 10.0 | 6.4 / 6.4 | +0.03 ± 0.61 | none strong enough |

± is one standard error over 30 pairs. Our decoder at t4/f2 adds 0.3–1.1 decodes per slot on both
arms. A second 20 m channel centred 2.8 kHz higher adds 5 messages to the window's 3,371 (0.1%).
On 20 m our decoder levels off near 27 decodes per slot whatever jt9 finds: the stations it misses
are those within about 30 Hz of a stronger one.

**SNR against jt9**, `python3 tools/decode_bench/score_snr.py 15m 20m 40m` (2026-11-03): our SNR minus jt9's
on the messages both decoded, the same file to each.

| Band | IQ at −20 dBFS | IQ at its own level | Audio |
| --- | --- | --- | --- |
| 15 m | +0.22, sd 2.07, 94% (995) | −0.46, sd 1.12, 99% (971) | +0.29, sd 2.52, 87% (1,032) |
| 20 m | −0.22, sd 2.81, 82% (1,609) | −0.62, sd 2.35, 87% (1,590) | −0.12, sd 2.88, 81% (1,594) |
| 40 m | +0.21, sd 2.37, 91% (385) | +0.15, sd 1.65, 96% (383) | +0.35, sd 2.53, 89% (383) |

Mean and sd in dB, then the share within 3 dB, and the message count.

## A full-band balance correction: tried, reverted

The same slots rescored with a blind DC and gain/phase correction estimated across the whole 48 kHz
window (`iq_convert --balance` at `c00fd97`, 1 s time constant, nothing tuned to this corpus; reverted
in the next commit because it mis-estimates on this radio):
`score_ab.py BAND_DIR --iq-flag=--balance --conv-dir conv-balance --jobs 6`. Uncorrected figures are the
table above; corrected / uncorrected where both are given.

| Band | jt9 per slot, IQ / audio | Ours per slot, IQ / audio | Paired IQ − audio per turn, jt9 | Image rejection, median (wrong-side decodes) | Converged gain, median (range) | Converged phase, median (range) |
| --- | --- | --- | --- | --- | --- | --- |
| 15 m | 24.4 / 25.5 | 16.5 / 17.1 (was 16.6) | −2.20 ± 0.91 (was −2.23 ± 0.90) | not measurable: 1 decode before, 0 after | −0.04 dB (−0.11 to −0.01) | +13.6° (−2.4 to +47.4) |
| 20 m | 56.1 / 55.9 (was 56.2) | 26.6 / 26.6 (was 26.8) | +0.43 ± 1.49 (was +0.53 ± 1.47) | 25 dB (531; range 10–46), was 32 dB (173) | +1.23 dB (+0.23 to +2.33) | +4.6° (+1.3 to +58.4) |
| 40 m | 10.0 / 10.0 | 6.4 / 6.4 | +0.00 ± 0.61 (was +0.03 ± 0.61) | none strong enough | −0.01 dB (−0.08 to +0.00) | +18.1° (−0.3 to +65.2) |

The 15 m gap did not close: IQ still trails audio by 2.2 jt9 decodes per slot (was 2.2), within the
error bars of no change. Nothing else moved outside the noise. On 20 m, the one band where the image
can be measured, the correction made it worse: more strong stations appear on the wrong side of the
LO (531 against 173) and at a lower rejection (25 dB against 32). The converged gain and phase are
each slot's last estimate. The phase is far larger and more scattered (up to 65°) than the 32 dB
uncorrected rejection allows (about 4°), and on 15 m and 40 m it sits near +14° to +18° at a gain of
0 dB. So the estimate is not the mixer's imbalance at the channel. Likely, untested: the correction as built
estimates across the whole 48 kHz window, where anything correlated between I and Q that is not the
channel's image (common-mode low-frequency content, an imbalance that differs with frequency across the
window) pulls it away; the image rejection spread of 20–50 dB across the 20 m window
already says the imbalance is not one number. The synthetic mixer in the host tests has a flat
imbalance, which is why they pass. Uncorrected IQ already matches audio on 20 m and 40 m, and the correction did not move the 15 m gap, so
balance is not what limits a single channel. It matters for two channels on one band, where FT8 and FT4
sit near each other's mirror (40 m); a correction then is estimated near the channel (after the NCO and
the first decimation, on the narrow channel's mirror term), not across the window (operator, 2026-11-02).

## FT4 at the window's top (2026-11-04)

With the dial on FT8's 14.074 MHz, 20 m's FT4 (14.080 MHz) is in these recordings 19.5 kHz above the LO, near the top of the QMX's 48 kHz window, where the ADC's own filter ends near 21.8 kHz. `python3 tools/decode_bench/score_ft4_edge.py 20m` decodes it there with `jt9 -5`, each 15 s slot cut into its two 7.5 s FT4 periods (`--ours` adds our own FT4 decoder: [decode.md](../../../../docs/architecture/decode.md), **FT4**):

| | |
| --- | --- |
| FT4 decodes | 980 in 120 periods of the 60 I/Q slots, 8.2 a period; every period had some |
| SNR | median 0 dB, weakest −21 dB; DT median 0.1 s |
| By audio offset, per 500 Hz (18.0 to 21.0 kHz above the LO) | 47, 179, 264, 250, 202, 38 |

FT4 decodes at that edge, and the offsets show the usual activity shape, dense in the middle and thin at both ends, with no cut-off toward the top. The recordings hold no FT4 away from the edge, so this shows it works there, not that it loses nothing.
