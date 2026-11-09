# QMX retune corpus, 2026-11-08

Row [xvfn](../../../../docs/roadmap/several-channels.md#xvfn) (FT4 replies on 40 m, below FT8's dial): does
a QMX's receive lose anything after its dial moves to FT4's and back for a transmission? The audio is not in
git. It is the release [`corpus-2026-11-08`](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-08),
one tar holding the run's 60 WAVs and its `manifest.jsonl`. The manifest here is a copy (`40m.jsonl`), so every
file's SHA-256 is in git.

| Tar | SHA-256 |
| --- | --- |
| `qmx-iq-retune-2026-11-08-40m.tar` | `1269db6b0686f3449b6ad4dfe75bc2aafb8280d4d9e0c4a5ec708145f21c79dd` |

## The station and the run

* QMX, firmware 1_04_010, a 40 m end-fed half-wave tuned for 40/20/15 m, the QMX's USB-C straight into a Linux
  workstation with NTP-synced time; IQ mode (`Q9 1;`), dial 7.074 MHz, Digi, VFO A.
* `tools/qmx_record.py --mode-tag iq --dial-hz 7074000 --minutes 15`: 60 slots, 03:06:15–03:21:00 UTC on
  2026-11-09 (21:06–21:21 CST on 2026-11-08), 48 kHz stereo 24-bit, 0 overflows. Its manifest's `recorder` reads
  `143a629-dirty`: the probe's changes, committed as `c318a2c`, were not yet in; `qmx_record.py` was unchanged.
* Alongside, `tools/qmx_tx_probe/key_qmx.py --mode FT4 --dial 7074000 --tx-dial 7047500 --via retune --no-key
  --every 8 --count 12`: at each minute from 03:07 to 03:18 UTC, `FA` to 7.0475 MHz a second before the minute
  and back to 7.074 at :05.5, where an FT4 transmission's `RX;` would go. Nothing was keyed. Every set read back
  in 30 or 31 ms, the probe's 30 ms gap included. Its output is `probe.log`.

## How it was scored, and what it found

`tools/qmx_tx_probe/score_retune.py` (its docstring): each slot through `tools/iq_convert` at +13,500 Hz (FT8)
and −13,000 Hz (FT4) from the LO, `--normalize-db -20`, decoded by `jt9 -8 -d 3` over the slot and `jt9 -5 -d 3`
over each 7.5 s half. Its output is `score.txt`.

* **FT8 after the dial came back** (:15) decoded 676 in 12 slots, 56.3 a slot, against 663, 55.2 a slot, in the
  slot of the same parity 30 s later (:45). Per minute, after minus control: −7, 0, +13, −4, +3, +4, −2, +2,
  −11, +4, −3, +14; mean +1.1, standard error about 2. The idle minutes' odd slots decoded 59.7 a slot.
* **The slot the dial left** (:00) decoded 21.9 a slot against 43.0 in the even control (:30): the 5.5 s the
  radio spent on FT4's dial, which our own transmission's deafness costs anyway.
* **FT4:** 3 decodes after a move against 4 in the control. 40 m had almost no FT4 that evening (PSKReporter,
  just after the run: 18 spots of 4 stations in 75 s worldwide, against 10,409 of 424 on FT8). Its one steady station,
  W2AAT, decoded in the half after a move as in the control half, at the same SNR.

Pairing within the minute matters: on 40 m that evening the odd slots carried more stations than the even ones
whether or not the dial moved, so an even slot is no control for an odd one.
