# QMX IQ-versus-audio on the P4, 2026-11-03

Stage 3 of row y9bt (decode from the QMX's IQ mode), on the device: the P4 decoding the QMX's I/Q
stream through `components/iq_frontend` against its demodulated audio, in the same run. Unlike the
[2026-11-02 corpus](../qmx-2026-11-02/README.md), nothing was recorded as audio: the evidence is the
station's own event feed. The raw logs are the release
[`corpus-2026-11-03-device`](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-03-device).

| File in the release | What |
| --- | --- |
| `ab-events.jsonl.gz` | every `/api/events` event of the A/B run, one JSON object per line, deduplicated by its `s` |
| `ab-schedule.log` | each band change (UTC), the config write's status and the band read back |
| `p4-ab.log.gz` | the P4's console for the run |
| `events.jsonl.gz`, `slots.log`, `p4-stage3.log.gz` | the earlier sequential runs (stage 3 steps 4 and 6) |

## The station and the run

* QMX, firmware 1_04_010 (a mid-band: 60 to 15 m), a 40 m end-fed half-wave; the P4 on `y9bt-iq-frontend` at `3359b6f`
  (`corpus-2026-11-02-21-g3359b6f`, not dirty); the sidekick on home WiFi.
* Station `qmx_iq_ab=2`: the receive path alternated every two slots (IQ for slot index / 2 even), switched over CAT
  (`Q9`) 0.5 s before each boundary. Each SLOT line names its path (`rx=iq` / `rx=au`).
* Bands (CST, UTC−6): 20 m 10:47–11:46, 15 m 11:47–12:46, 17 m 12:47–13:46, 30 m 13:47–14:46, 60 m 14:47–15:01 (stopped:
  no decodes in either mode), 40 m 15:01–16:53. 17 m, 30 m and 60 m were receive-only for the antenna.
* **Four transmissions:** Settings' band-change amplifier priming (`amp_prime_on_band_change=1`) keyed a 500 ms SWR Tune
  carrier at each band change from 15 m to 60 m. It was switched off at 14:57 CST; 40 m's change sent nothing.
* The switching: 736 `Q9` sends, every read-back matching, none undelivered; three switches taken back, all at the
  10:47 reboot and clock set. `drop` 0 throughout, `lost` at most 2,640 samples, on quiet-band slots in both modes.

## How it was scored

`python3 tools/decode_bench/score_device_ab.py ab-events.jsonl ab-schedule.log 2026-11-03`. A slot counts once the clock
is set and on the band the schedule had it on, leaving out the slot a band change lands in. Turns are runs of slots in
one path; each turn is paired with the next turn of the other path, so a steady band trend cancels. Pooling weights
each band by its own precision.

| Band | IQ decodes/slot | Audio decodes/slot | Paired IQ − audio | Past the boundary, IQ / audio |
| --- | --- | --- | --- | --- |
| 20 m | 20.25 (118) | 20.08 (117) | +0.17 ± 0.19 (+0.8%) | 0 / 0 |
| 15 m | 6.61 (119) | 6.44 (119) | +0.17 ± 0.13 (+2.6%) | 44 / 38 |
| 17 m | 9.74 (119) | 9.73 (119) | +0.01 ± 0.16 (+0.1%) | 17 / 21 |
| 30 m | 5.95 (119) | 5.92 (119) | +0.03 ± 0.12 (+0.6%) | 43 / 42 |
| 40 m | 11.54 (224) | 11.32 (222) | +0.23 ± 0.11 (+2.1%) | 35 / 29 |

± is one standard error. **Pooled: IQ +1.29% ± 0.57%** (95% +0.2 to +2.4%, p 0.025), five bands of five positive.

* **The front end's cost:** BDRY `fe` 1.261 s of core 0 per IQ slot (8.4%), core 0 12% busy on IQ slots against 4% on
  audio. It costs the decode nothing: overruns are as frequent on either path.
* **Below 300 Hz:** IQ 0.15 decodes a slot, audio 0.13, with the front end's passband from 150 Hz. With it from 200 Hz
  (the sequential runs) IQ had 0.01 against audio's 0.34.
* **After a switch:** the first slot of a turn decodes no worse than the second (IQ 10.59 against 10.24, audio 10.46
  against 10.11 a slot).
* **The sequential runs** (key on for 30 minutes, then off, then in the other order, `slots.log`): IQ 7 to 12% behind,
  −9.1% pooled. Interleaved, that gap is gone: the 200 Hz edge and band drift between the halves, not the path.
* **Quiet bands overrun in both paths** (15 m 35%, 30 m 36%, 40 m 14% of slots; 20 m none): what the decode budget
  now bounds ([decode.md](../../../../docs/architecture/decode.md), **The work budget**).
