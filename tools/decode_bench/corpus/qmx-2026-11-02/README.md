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

IQ files through `tools/iq_convert` (`--center-hz 13500`, I on the left channel, Q not negated, no
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
