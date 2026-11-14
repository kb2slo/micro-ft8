# Station events, 20 m operating, 2026-11-10

An hour and a quarter of operating FT4 on 20 m, with a few FT8 replies in the same queue: the station's own
`/api/events` stream, as Operate saw it, recorded while the operator worked the band. It is the evidence for the
roadmap's key-up item ([several-channels.md](../../../../docs/roadmap/several-channels.md), **Open**), the queue
items ([smart-queue.md](../../../../docs/roadmap/smart-queue.md), **Later** and **Open**), the offset a QSO holds
([autoseq.md](../../../../docs/architecture/autoseq.md), **TX offset**) and idea [yn23](../../../../docs/roadmap/ideas.md#yn23), and for
[decode.md](../../../../docs/architecture/decode.md)'s **On the air, 20 m while operating**.

## The station and the recording

* QMX 60–15 m, firmware 1_04_010, IQ mode, a 40 m end-fed half-wave tuned for 40/20/15 m; the micro PA at about
  25 W, SWR 1.22–1.27. Main board, sidekick and web app all `ceecbb6`. Dial 14.074 MHz with **Also decode FT4**
  on, FT4 transmitted as a far `TA` tone at 14.080 MHz plus the offset. Tries 3, TX offset random.
* `events.jsonl.gz`: every event the sidekick served from 21:18:30 to 22:39 UTC (15:18–16:39 CST), one JSON
  object per line, deduplicated by sequence number `s`. The band moved to 40 m at 22:38:38. Recorded by a
  headless browser holding Operate open, which logged each `/api/events` reply; for the first three minutes by a
  script polling `/api/events` every 3 s. Activity rows (`"d":8`, one a second) are left out; nothing here reads
  them.
* **One gap:** at 22:11:06 `micro-ft8.local` stopped resolving for the recording computer and the operator's phone
  at once, for about half a minute, while the sidekick answered by IP; the recorder resumed by IP. The events of
  roughly that half-minute are missing (`s` jumps past the sidekick's 128-event ring).
* What the events mean: [sidekick.md](../../../../docs/architecture/sidekick.md), **Port A**; the `SLOT`, `BDRY`
  and `STALL` log lines: [decode.md](../../../../docs/architecture/decode.md), **The time budget**.

## How it is scored, and what it shows

```bash
python3 tools/decode_bench/score_events.py tools/decode_bench/corpus/station-2026-11-10/events.jsonl.gz \
    --from 21:19:00 --to 22:38:38 > tools/decode_bench/corpus/station-2026-11-10/score.txt
python3 tools/decode_bench/score_events.py tools/decode_bench/corpus/station-2026-11-10/events.jsonl.gz \
    --from 21:39:00 --to 21:44:00 --timeline D2UY
```

Its output for 20 m is `score.txt`.

* **Decode load was ordinary.** FT4 decoded 419 periods, 0–12 messages (mean 5.3), 299 at their budget; FT8 111
  slots, 17–31 (mean 24.6), none at its budget. The 2026-11-07 hour on 20 m, mostly listening, decoded more (FT4
  mean 6.8, 83% at the budget).
* **What differed was 207 transmissions, and the faults came with them.** `STALL` came 14 times, 58–136 ms, none
  with empty reads, and each wait ended within 0.7 s of a transmission starting (the status row's time is whole
  seconds); 13 were under 75 ms. Two of the three decodes
  that ran past a boundary (FT4 −96 ms, FT8 −55 ms) ended in the second a transmission started; the third (FT4
  −38 ms) did not. The 2026-11-07 hour had neither.
* **The report we send** is not the reply's SNR when the operator tapped the station first (it is latched from
  the tapped decode, [autoseq.md](../../../../docs/architecture/autoseq.md)): of 22 reports we sent, 13 differ
  from the reply they answered, by up to 12 dB (KF0KBL R-1 to a reply at +11; N6UA R+0 to +9; D2UY R+3 to +10).
* **The offset moves every transmission,** within a QSO too: D2UY's three R-reports went out at 2317, 702 and
  1141 Hz, and no RR73 came.
* **Waits in the queue:** a station tapped while another QSO was further along took the slots, and earlier taps
  waited: AA8TJ 495 s between its first and second call, W8CTO 390 s, WA2DX 300 s; KB3UJT, after answering us,
  75 s between our two R-reports while a later tap took four periods.
* **FT4 calls deafen FT8 replies.** The D2UY timeline: after our third R-report at 21:42:45, the FT8 slots from
  21:42:58 to 21:43:58 were all skipped, the station transmitting FT4 calls in them, so D2UY's RR73 could not be
  heard had it come.

## Limits

* One operator's session, every call a tap, so the queue items are cases, not rates.
* The scorer's "reply" is the newest decode from that station to us before our report; where a station called us
  more than once, it is the latest.
* A transmission's start is the first status row with it active, stamped in whole seconds, so "within 0.7 s"
  is as close as the log can place it.
