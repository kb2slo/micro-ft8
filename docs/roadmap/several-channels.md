# Several channels at once (epic u8mc)

Receive, decode and transmit on 1–N channels at once, whatever the radio. A channel is a source, a dial, a band and a mode; one QMX gives FT8 and FT4 on one band, one HL2 several bands. Rows: [xvfn](#xvfn); decode on both cores, and FT4 received beside FT8 from one QMX ([decode.md](../architecture/decode.md), **FT4**), shipped on 2026-11-07, and FT4 replies beside FT8 on 2026-11-08 ([autoseq.md](../architecture/autoseq.md), **The queue**) ([decode.md](../architecture/decode.md), **Channels**) ([ROADMAP.md](../ROADMAP.md)). The HL2's own side is epic [knpp](hl2-support.md).

## Goal

**Decoding more channels degrades, never overruns** (operator, 2026-11-02): one channel gets the deepest decode the slot allows, each channel added takes depth from the others in order of what is worth least, and no slot's decode runs past its deadline. **And every channel is operated, not only decoded:** a tap on a decode on any channel completes a QSO on that channel, in its mode, and logs it there. The first case is FT8 and FT4 at once from one QMX.

## Constraints

* **The QMX station decodes exactly as today** while it has one FT8 channel and time to spare: the recall floors and the `tx_e2e` goldens hold.
* **Deterministic under test.** The host replays any mix of channels and gets the P4's output byte for byte; CI does not depend on the host's speed.
* **No mode's timing is built in** (several digital modes are the vision, row [I6](ideas.md#I6)); this epic builds FT8 and FT4 only, and nothing assumes a 15 s slot or even/odd periods.
* **No radio is built in:** a channel names its source; the scheduler, autoseq and the log never ask which radio it is.
* **No board count is built in:** one P4 here; several are a direction (**Why not**).
* **Each QSO on the air is a WSJT-X QSO** ([AGENTS.md](../../AGENTS.md), **WSJT-X is the on-air standard**), and **one transmission at a time**, on one channel.

## Decided

* **Each channel has its own deadline and work runs earliest deadline first,** so modes that are not 15 s even/odd fit.
* **Admission on each mode's declared minimum,** not "always a first pass": a channel the budget cannot carry is refused when it is added, with a reason the web app shows. With several boards admission is placement, the QSO partner's channel on the station board.
* **Within a board, units of work share the cores; across boards, whole channels.** A network round trip costs more than a unit, and an order that depended on the network could not be replayed.
* **The budget is counted, not timed;** the wall clock is a backstop that logs an overrun and means the cost table is wrong.
* **The unit is an LDPC iteration,** which our `ft8_lib` fork reports, plus a fixed cost per search and per candidate, each measured on the P4 with `decode_bench`; the margin is fixed by those costs. Built for one channel ([decode.md](../architecture/decode.md), **The work budget**).
* **The minimum may lose its weakest candidates:** on a quiet band the first search alone overran, so the budget can stop inside it, and candidates come in score order, so it drops the tail. The first search itself always runs.
* **The QMX's receive dial is FT8's** (operator, 2026-11-07), FT4's channel beside it wherever the window holds it, and on 15 m and 10 m nowhere: [radio.md](../architecture/radio.md), **The QMX's receive output**. Transmitting keeps it where the channel is above it: a QMX takes a `TA` tone as far as FT4's dial, so no retune ([radio.md](../architecture/radio.md), **TX timing on a QMX**). Below it, the next bullet.
* **Below the receive dial, a QMX retunes for the transmission** (operator, 2026-11-08), as WSJT-X's *Fake It* does: `FA` to the channel's dial a second before the slot, and back to the receive dial just after `RX;`. Not split with VFO B. Measured on 40 m on QMX 1_04_010 from a computer (`tools/qmx_tx_probe/`), FT4 aimed at 7.0475 MHz plus 1,500 Hz:
  * **Where it lands:** 7.048995 MHz by retune (5 spots; most receivers had just reported the split run) and 7.048997 MHz by split (91 spots, 7.048953–7.049063). Each `FA` is answered in about 1 ms.
  * **What receive loses after it:** with the dial moved at an FT4 transmission's times and nothing keyed, once a minute for 12 minutes (`key_qmx.py --no-key`, scored by `score_retune.py`), FT8 decoded 56.3 a slot in the slot after the dial came back, against 55.2 in the slot of the same parity 30 s later (paired by minute, mean +1.1, standard error about 2). The slot the dial left decoded half as many, which our transmission's deafness already costs. FT4 had almost no traffic to count (5 stations worldwide on 40 m that evening); its one steady station decoded after a move as in the control half.
  * **Why not split:** it landed as precisely, but after `FB<dial>;` `SP0;` `FR0;` `FT0;` the radio's display kept showing A and B. The radio was on A: every read back said so, and an FT8 CQ sent with B set elsewhere landed on A's frequency. Neither those nor `FR1;` then `FR0;` a second later redrew it, the only other commands tried; one press of the VFO button did. Split also takes about six commands a transmission against two. On 1_04_010, unlike qmx-panadapter's notes, the radio took `FB` with split off, and `FB;` answered `FB…`.
* **Decode leaves the capture task,** rather than every channel's ring growing: decoded in turn inside the capture task, each ring would have to hold the sum of every channel's decode, which grows with the channels and on a quiet band, and all decode would stay on one core. Decode subtracts from the waterfall it reads, so capture needs another for the blocks after the trigger: each channel has two in turn (161,076 bytes more PSRAM each), decode owns the one it subtracts from, and the ring covers USB jitter only. With decode due before our next transmission, about the boundary, the second holds the slot's discarded tail; it earns its place when a decode runs late (a slot is dropped, not capture held), when the decode start moves later ([decode.md](../architecture/decode.md), **Decoding later in the slot**), or for **Decode twice a slot** (**Later**). Built, with two workers ([decode.md](../architecture/decode.md), **Channels**).

## Design

### A channel, and its sources

* **A channel** is a source of samples, a dial, a band and a **mode**. Its capture and decode state is its own, the FFT plan and window included ([decode.md](../architecture/decode.md), **Channels**). Two channels may share a band (FT8 and FT4 on one QMX) or a mode (FT8 on two HL2 receivers).
* **A source** is what a radio backend hands the pipeline: one stream, its rate, and for each channel it carries the dial, a dropped-sample count and readiness. **An I/Q source** carries one or more channels, each an I/Q front-end instance at its frequency ([decode.md](../architecture/decode.md), **I/Q front end**): one QMX is one, FT8 and FT4 two instances on it; one HL2 receiver is one. **A real-audio source** carries one channel, as a QDX would.
* **Per-source limits are the source's:** which channels fit a QMX's window, what an HL2's shared filter does. Admission refuses a channel the source cannot carry, with the source's reason.
* **Per channel:** the dial against the radio's and the front-end instance are a channel's own, beside its buffers ([decode.md](../architecture/decode.md), **Channels**, **FT4**; [memory.md](../architecture/memory.md), **A channel's buffers**): 438 KB of PSRAM a channel with its two waterfalls; internal: a hand-off of about 7 KB; the QMX's FT4 front end (7,528 bytes) is in PSRAM.

### The scheduler

* **A mode declares its timing and its work.** Timing: *slotted* (period, trigger point, deadline) or *streaming*; FT8 is slotted at 15 s, FT4 at 7.5 s, and the trigger point is per mode. Work in steps, each with a cost (one search pass, one candidate decoded). Its **minimum**, the work without which the slot is worthless (the first pass and as many of its candidates as fit, **Decided**). When it is deaf: while we transmit.
* **Order:** first the QSO partner (its channel, and on it the candidates near the partner's tone, so a reply is decided in time for the next slot); then every channel's minimum, earliest deadline first (an FT4 deadline falls twice in each FT8 slot); then deeper work, round-robin, earliest deadline first, each kind placed by its measured gain per cost (rows [kkap](../ROADMAP.md#kkap), [n2e7](../ROADMAP.md#n2e7), [qauk](../ROADMAP.md#qauk) add kinds).
* **The budget in units the host can replay:** for one channel, built ([decode.md](../architecture/decode.md), **The work budget**). With several, a slot's budget is its time to its deadline less the margin, in units.
* **Two cores:** one queue of units, a worker on each core. Core 0 also carries USB or Ethernet and the front end; its share is what remains after those, measured.
* **Visibility:** each slot's SLOT line reports, per channel, the work done, the depth reached (minimum only, which pass) and whether the backstop fired; a channel that ran only its minimum is visible to the operator and in the logs.

### Operating several channels

* **Every queue entry carries its channel** (band, mode, dial) beside its offset and slot parity, which is in its mode's periods. A reply goes out on the channel the decode was heard on; autoseq advances a QSO only on decodes from that channel, so one station heard on two channels is two candidates.
* **One setting chooses the channels listened to,** within what admission allows, **and one the channel we CQ on.**
* **The log** takes the QSO's band, mode and frequency from its channel, and the radio's name from the backend.
* **The web app** shows each channel's decodes as their own batch and says which channel a QSO is on; grouping and filtering build on [webapp.md](../architecture/webapp.md), **Decode sort and filter**. Losses show per channel: gaps, zero-fill, ring drops, depth.

### Transmit

* **One channel at a time;** it sets the frequency, the mode's waveform and timing, and the slot.
* **No transmit band table** (operator, 2026-11-07): the frequency is the operator's, as the licence is. A decode chooses where we transmit only within the band the operator tuned, so each transmission's tones are held to 100 Hz to 10 kHz from the dial, which catches a wild offset without being a band table ([autoseq.md](../architecture/autoseq.md), **The slot cycle**).
* **Deaf by overlap, not by slot index.** A channel is deaf for each of its slots our transmission overlaps, on any channel of the station: an FT8 transmission deafens both FT4 slots inside it, an FT4 one the FT8 slot it falls in. Today's `tx_slot_idx` skip is the one-channel case and stays byte-identical. The web app says that a channel calling in our slots is invisible until we stop. Built for FT8 and FT4 on one QMX ([decode.md](../architecture/decode.md), **FT4**).
* **On a QMX, a channel off the receive dial is a far `TA` tone,** not a retune: the radio transmits at the dial plus the tone, and takes FT4's 7,500 Hz from FT8's dial ([radio.md](../architecture/radio.md), **TX timing on a QMX**). That holds where FT4's dial is above FT8's; `TA` cannot go below the dial, so on 40 m, where FT4 is 26.5 kHz below, the transmission retunes and returns (**Decided**, row [xvfn](#xvfn)).

## Children

Only the next child is a row in the index ([ROADMAP.md](../ROADMAP.md), Now: [xvfn](#xvfn)); the others wait here, in order, until it ships.

* <a id="xvfn"></a>**xvfn: FT4 replies on 40 m, below FT8's dial.** On 40 m FT4's dial, 7.0475 MHz, is 26.5 kHz below FT8's, and a `TA` under 10 Hz keys the radio up, so FT4 cannot go out from FT8's dial as a far tone ([radio.md](../architecture/radio.md), **TX timing on a QMX**); until this row, an FT4 reply there is refused with a reason. The way was settled on the bench on 2026-11-08, where the RF lands, how long the radio takes and what receive loses after it measured from a computer (**Decided**): a retune with `FA` for the transmission and back. Moving the receive dial to FT4's does not serve: FT8's channel would sit about 40 kHz above the LO, outside the window, and today's dial already sits between the two. **Done-when:** the chosen way measured on the bench on QMX 1_04_010, where the RF lands, how long the radio takes and what receive loses after it (field); a tap on an FT4 CQ on 40 m completes a QSO logged as MFSK/FT4 at 7.0475 (field). **Lands in:** radio.md (**TX timing on a QMX**); this file's **Open** settled.

**Later** (slices not yet rows):

* **FT4 CQ and the CQ's channel:** calling CQ in FT4, and a setting for which channel the CQ uses; today the CQ is FT8's.
* **Several bands from one HL2:** HL2 receivers as I/Q sources (epic [knpp](hl2-support.md), **Receive**), two or more bands as admission allows, a reply on the band heard through the path FT4 replies use ([autoseq.md](../architecture/autoseq.md), **The slot cycle**); after the HL2's own receive shipped. **Done-when:** two bands decode for an hour with every loss counter at zero or explained, and a CQ on the second band completes a QSO there (field); or the finding that one P4 carries one HL2 band. Sharing the N2ADR's filter cost 10, 15 and 20 m nothing measured ([hl2.md](../architecture/hl2.md), **Receive**).
* **A QMX and an HL2 at once:** channels from each, a reply transmitted by the radio whose channel heard it; one backend per radio in place of one selected backend (epic [knpp](hl2-support.md), **Open**).
* **A recorded band replayed into the P4 as an HL2:** a computer on the P4's Ethernet port answers as the HL2 and sends a recorded stream in Protocol 1, from the [2026-11-04 survey](../../tools/decode_bench/corpus/hl2-2026-11-04/README.md)'s quiet bands or epic [knpp](hl2-support.md)'s mirror-port capture (exactly what the P4 decoded live). An hour on any band then runs on the bench on demand, and two builds' slot times and decodes compare on identical input. Receive only, and the replayer is the radio, so nothing can key. Becomes a row if a field Done-when should accept a replayed hour.
* **More decode per core,** if the budget needs it after two workers: hot loops in internal RAM or the P4's 8 KB TCM; SIMD for the candidate search over the `uint8` waterfall (the vector units are integer-only, [decode.md](../architecture/decode.md), **What the chips offer**); an early stop to LDPC, chosen from [decode.md](../architecture/decode.md)'s **Stopping LDPC early** table. Each scored on the recall harness and timed on the P4.
* **Decode twice a slot:** a late, cheap pass that only decides the reply (in a QSO, candidates near the partner's offset, as AP decoding is), and the full decode after the boundary, off the capture task, with nearly a slot when we transmit next. Decodes would then appear in two waves, as in WSJT-X; whether the operator accepts that is open. It would change the decode starts ([decode.md](../architecture/decode.md), **Decoding later in the slot**).

## Open

* **What a busy slot costs beyond its units** (2026-11-07, 40 m): a decoded message adds its unpack, SNR and subtraction, which the budget does not count (`DECODE_COST_CANDIDATE` is 0, `decode_limits.h`), so a busy slot runs slower per unit than the quiet ones the on-air rate came from. FT8 alone at its 7,300 cap kept 66 ms, not the 100 ms the budget aimed for. Measuring a decode's cost on the P4 bench and charging it would make the budget hold on busy bands as on quiet ones.
* **Whether FT8's cap beside FT4 costs decodes:** on busy 40 m FT8 hit its 6,500 units in every slot (81 of 81) and still found 2.8 messages a slot against 2.6 alone, but the air has no truth to count what the cap missed. The host found nothing lost at 6,000 on the corpora and the HL2 captures; a capture beside the P4 on a busy 40 m evening would settle it.
* **The audio holes with FT4 on** (2026-11-07, 20 m, 12:10 CST): 1.72 s and 0.28 s missing from the QMX stream with nothing dropped in our ring, none since in about two hours of either build, nor in the hour both decoded on 20 m that evening. `STALL` (`ft8_audio_pipeline.cpp`) says whether ingest stalled or the radio sent nothing when it recurs ([decode.md](../architecture/decode.md), **The time budget**).
* **What the operator sees** when admission refuses a channel, and when a channel ran only its minimum.

## Facts

Read from the working tree at `ef4e271`.

* **One protocol, set once.** `g_protocol` is set at boot (`poe_p4/main/main.cpp`) and is the one channel's mode; receive reads only the channel's, and transmit still reads `g_protocol` (`poe_p4/main/tx_scheduler.cpp`, `poe_p4/main/porta_host.cpp`). Capture and decode state is per channel (2026-11-04, [decode.md](../architecture/decode.md), **Channels**).
* **Decode runs inside the capture task,** pinned to core 1, starting after 79 blocks (12.64 s), leaving a 2.36 s guard; an ingest task fills a 2.7 s ring meanwhile. Our TX slot is skipped by slot index (`tx_slot_idx`, `ft8_audio_pipeline.cpp`).
* **One band and one mode throughout the station.** One `band_sel` (`components/station/include/station.h:22`); a transmission goes on `porta_p4_station_freq_hz()` (`poe_p4/main/tx_scheduler.cpp:255`); autoseq's `QsoContext` and `AutoseqTxEntry` carry an offset and an even/odd `slot_id` but no band and no mode (`components/autoseq/include/autoseq.h:102`, `:120`); the log takes band and frequency from `band_sel` at log time, and `radio_name = "QMX"` is fixed (`poe_p4/main/porta_host.cpp:299`). FT4 is saved and ignored.
* **Already several-channel-ready:** Port A's DECODE carries the dial per decode (`components/porta_proto/include/porta_proto.h:264`), and the web app splits a slot's decodes into one batch per frequency. The I/Q front end is one instance per channel, caller-owned, with no statics, 8.4% of core 0 on the P4.
* **The QMX's window:** one band-pass filter and one LO; 48 kHz I/Q from 36 kHz below the dial to 12 kHz above (the LO 12 kHz below the dial). On 20 m FT4's dial (14.080) is 6 kHz above FT8's, so with the dial at FT8's its channel is 19.5 kHz above the LO; on 40 m each mode sits near the other's mirror, separated by the front end's image rejection (about 32 dB on a QMX, uncorrected). The QMX transmits at the dial plus the `TA` tone.
* **Cost.** A busy 20 m slot decodes in about 1.75 s of one core, a quiet one in 2.15–2.3 s. **Recall plateaus on crowded bands** ([corpus-2026-11-02](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-02)): about 67% of `jt9` at 25 messages a slot, 47% at 56; more candidates, a third pass or more LDPC iterations each move it under 2%, t4/f2 2–4%. So depth beyond today's two passes buys little until subtraction improves (row [kkap](../ROADMAP.md#kkap)), and a second channel's first pass is worth far more than either channel's third.

## Why not

* **Fixed tiers by channel count:** cost depends on the band more than the count (a quiet band costs more than a busy one), so a tier is too cautious on a busy evening and still overruns on a quiet one.
* **A wall-clock deadline as the policy:** the output would depend on CPU speed, so the host could not replay it and CI's floors would drift.
* **One core per channel:** one channel would use half the P4, and two could not borrow from each other.
* **Units of work across boards:** each would wait on a round trip costing more than the unit, and the order would depend on the network.
* **One decoder per mode, each its own task:** FreeRTOS priorities would decide which band goes dark, invisibly.
* **Hopping one QMX between bands each slot over CAT:** each band heard half the time, and each slot after a hop starts with a retune. **Several bands on one QMX:** one band-pass filter and one LO; the QMX+ is the same design.
* **Receiving while we transmit:** the radio's front end sees our transmission; WSJT-X is half-duplex too. **Several QSOs at once:** a transmitter per channel or interleaved transmissions, and neither is a WSJT-X QSO.
* **Several P4s joined:** whole channels placed per board would scale, but worth designing only once one P4's capacity is measured; two workers' numbers ([decode.md](../architecture/decode.md), **The work budget**) say what one carries.
* **Other modes now:** WSPR (slotted at 2 minutes) or CW and RTTY (streaming) fit the same declarations, but are row [I6](ideas.md#I6)'s; each needs its own MIT implementation, since the obvious reference decoders (JS8Call's tables, `wsprd`) are GPL.
