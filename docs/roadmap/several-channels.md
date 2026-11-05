# Several channels at once (epic u8mc)

Receive, decode and transmit on 1–N channels at once, whatever the radio. A channel is a source, a dial, a band and a mode; one QMX gives FT8 and FT4 on one band, one HL2 several bands. Rows, in order: [d9me](#d9me), [vnz7](#vnz7), [an8x](#an8x), [grqg](#grqg) ([ROADMAP.md](../ROADMAP.md)). The HL2's own side is epic [knpp](hl2-support.md).

## Goal

**Decoding more channels degrades, never overruns** (operator, 2026-11-02): one channel gets the deepest decode the slot allows, each channel added takes depth from the others in order of what is worth least, and no slot's decode runs past its deadline. **And every channel is operated, not only decoded:** a tap on a decode on any channel completes a QSO on that channel, in its mode, and logs it there. The first case is FT8 and FT4 at once from one QMX.

## Constraints

* **The QMX station decodes exactly as today** while it has one FT8 channel and time to spare: the recall floors and the `tx_e2e` goldens hold.
* **Deterministic under test.** The host replays any mix of channels and gets the P4's output byte for byte; CI does not depend on the host's speed.
* **No mode's timing is built in** (several digital modes are the vision, row [I6](../ROADMAP.md#I6)); this epic builds FT8 and FT4 only, and nothing assumes a 15 s slot or even/odd periods.
* **No radio is built in:** a channel names its source; the scheduler, autoseq and the log never ask which radio it is.
* **No board count is built in:** one P4 here; several are a direction (**Why not**).
* **Each QSO on the air is a WSJT-X QSO** ([AGENTS.md](../../AGENTS.md), **WSJT-X is the on-air standard**), and **one transmission at a time**, on one channel.

## Decided

* **Each channel has its own deadline and work runs earliest deadline first,** so modes that are not 15 s even/odd fit.
* **Admission on each mode's declared minimum,** not "always a first pass": a channel the budget cannot carry is refused when it is added, with a reason the web app shows. With several boards admission is placement, the QSO partner's channel on the station board.
* **Within a board, units of work share the cores; across boards, whole channels.** A network round trip costs more than a unit, and an order that depended on the network could not be replayed.
* **The budget is counted, not timed;** the wall clock is a backstop that logs an overrun and means the cost table is wrong.

## Design

### A channel, and its sources

* **A channel** is a source of samples, a dial, a band and a **mode**. Today's single-instance state (**Facts**) becomes per channel; the FFT plan and window are shared. Two channels may share a band (FT8 and FT4 on one QMX) or a mode (FT8 on two HL2 receivers).
* **A source** is what a radio backend hands the pipeline: one stream, its rate, and for each channel it carries the dial, a dropped-sample count and readiness. **An I/Q source** carries one or more channels, each an I/Q front-end instance at its frequency ([decode.md](../architecture/decode.md), **I/Q front end**): one QMX is one, FT8 and FT4 two instances on it; one HL2 receiver is one. **A real-audio source** carries one channel, as a QDX would.
* **Per-source limits are the source's:** which channels fit a QMX's window, what an HL2's shared filter does. Admission refuses a channel the source cannot carry, with the source's reason.
* **Per channel:** a ring, a monitor, block gains, the slot state and the dial. In PSRAM: its waterfall (161,076 bytes at t2/f2) and capture path (about 113 KB), so four channels are about 1.1 MB of 32 MB; internal: two slot results of about 7 KB, its front-end instance (7,528 bytes) and, if it gets one, a task stack ([memory.md](../architecture/memory.md)).
* **The ring.** Decode runs inside the capture task and the ring holds 2.7 s at 6 kHz; with several channels each ring grows (PSRAM, no internal RAM) or decode leaves the capture task: [d9me](#d9me) chooses.

### The scheduler

* **A mode declares its timing and its work.** Timing: *slotted* (period, trigger point, deadline) or *streaming*; FT8 is slotted at 15 s, FT4 at 7.5 s, and the trigger point is per mode. Work in steps, each with a cost (one search pass, one candidate decoded). Its **minimum**, the work without which the slot is worthless (the first pass and its candidates). When it is deaf: while we transmit.
* **Order:** first the QSO partner (its channel, and on it the candidates near the partner's tone, so a reply is decided in time for the next slot); then every channel's minimum, earliest deadline first (an FT4 deadline falls twice in each FT8 slot); then deeper work, round-robin, earliest deadline first, each kind placed by its measured gain per cost (rows [kkap](../ROADMAP.md#kkap), [n2e7](../ROADMAP.md#n2e7), [qauk](../ROADMAP.md#qauk) add kinds).
* **The budget in units the host can replay:** LDPC iterations run, plus a fixed cost per search pass and per candidate considered, each unit's P4 cost measured once with `decode_bench`. A slot's budget is its time to the deadline less a margin, in units.
* **Two cores:** one queue of units, a worker on each core. Core 0 also carries USB or Ethernet and the front end; its share is what remains after those, measured.
* **Visibility:** each slot's SLOT line reports, per channel, the work done, the depth reached (minimum only, which pass) and whether the backstop fired; a channel that ran only its minimum is visible to the operator and in the logs.

### Operating several channels

* **Every queue entry carries its channel** (band, mode, dial) beside its offset and slot parity, which is in its mode's periods. A reply goes out on the channel the decode was heard on; autoseq advances a QSO only on decodes from that channel, so one station heard on two channels is two candidates.
* **One setting chooses the channels listened to,** within what admission allows, **and one the channel we CQ on.**
* **The log** takes the QSO's band, mode and frequency from its channel, and the radio's name from the backend.
* **The web app** shows each channel's decodes as their own batch and says which channel a QSO is on; grouping and filtering is [RFC 0009](../rfcs/0009-operate-station-surface.md)'s rows. Losses show per channel: gaps, zero-fill, ring drops, depth.

### Transmit

* **One channel at a time;** it sets the frequency, the mode's waveform and timing, and the slot.
* **The frequency is checked before key-down,** by every backend: the dial plus the offset, and the signal's bandwidth, against a band table; host-tested, including an entry whose band and frequency disagree. A decode now chooses where we transmit.
* **Deaf by overlap, not by slot index.** A channel is deaf for each of its slots our transmission overlaps, on any channel of the station: an FT8 transmission deafens both FT4 slots inside it, an FT4 one the FT8 slot it falls in. Today's `tx_slot_idx` skip is the one-channel case and stays byte-identical. The web app says that a channel calling in our slots is invisible until we stop.
* **On a QMX, a channel off the receive dial costs a retune.** The radio transmits at the dial plus the `TA` tone: replying on FT4 at 14.080 MHz with the dial at 14.074 asks `TA` for a tone about 6 kHz above the usual offsets, if the radio takes one, or moves the dial for the transmission and back, moving the receive window with it (**Open**).

## Children

* <a id="d9me"></a>**d9me: per-channel state.** Today's single-instance capture and decode state per channel, with one FT8 channel; `g_protocol` out of the decode path; whether rings grow or decode leaves the capture task. **Done-when:** the recall floors and `tx_e2e` goldens unchanged byte for byte (category 1); a QMX decodes and transmits as before (field). **Lands in:** [decode.md](../architecture/decode.md) (channels and sources), [memory.md](../architecture/memory.md) (state per channel).
* <a id="vnz7"></a>**vnz7: the budget and the order, then two cores.** After [d9me](#d9me). Work counted, earliest deadline first, the minimum, the backstop, the SLOT line; then two workers, with subtraction's order decided on the recall harness (**Open**). Quiet bands overrun today: on 15 m (2026-10-31) 24 of 207 slots ended past the 2.36 s guard, the worst by 269 ms, since failing candidates run LDPC to its limit; during decode one core reads 2% busy, the other 100%. **Done-when:** host tests replay a mix of channels to identical output, and an ample budget reproduces [d9me](#d9me)'s output exactly (category 1); an hour on a quiet band with no slot past the guard; busy slots well under 1,747 ms; recall within the CI floors; and the decode start chosen per mode by a host sweep with the candidate cap raised ([RFC 0006](../rfcs/0006-decode-performance.md) §4.3) (field). **Lands in:** [decode.md](../architecture/decode.md) (the scheduler, the SLOT line), [main_board.md](../architecture/main_board.md) (**Tasks**). Its two-core half could follow [an8x](#an8x) instead of preceding it if one core carries two channels inside the budget.
* <a id="an8x"></a>**an8x: FT8 and FT4 at once from one QMX, receive.** After [vnz7](#vnz7)'s budget. Two front-end instances on the QMX's I/Q; FT4 decode on the P4 at all (the P4 runs FT8 whatever `protocol_mode` says); each mode's timing and deadline; where the receive dial sits so both fit the window (**Open**); each channel's losses and depth shown. **Done-when:** a recorded 20 m I/Q slot decodes both modes on the host to the same messages as each mode alone, in replay (category 1); on the air, both channels decode for an hour on 20 m with every slot inside its deadline, each channel's depth logged, and FT8's decodes within its recall of FT8 alone (field). **Lands in:** decode.md (FT4, two channels on one source), [radio.md](../architecture/radio.md) (the QMX's receive dial), [webapp.md](../architecture/webapp.md) (per-channel batches and losses); README and the site, FT4.
* <a id="grqg"></a>**grqg: reply on the channel heard.** After [an8x](#an8x). Queue entries with a channel; autoseq matching on it; the log's band and mode; the CQ channel setting; deafness by overlap; the band table; FT4 transmit; the QMX's retune or wide `TA`, decided on the bench. **Done-when:** autoseq, the queue, the overlap rule and the band table in `host_mock` (category 1); listening on FT8 and FT4 on one QMX, a tap on a CQ on each completes a QSO in that mode and logs it with its mode, and the FT8 channel is deaf only in the slots the overlap rule names (field). **Lands in:** [autoseq.md](../architecture/autoseq.md) (the channel on queue entries), radio.md (transmit off the receive dial), webapp.md, [logging.md](../architecture/logging.md) (mode); README and the site.

**Later** (slices not yet rows):

* **Several bands from one HL2:** HL2 receivers as I/Q sources (epic [knpp](hl2-support.md), **Receive**), two or more bands as admission allows, a reply on the band heard through [grqg](#grqg)'s path; after the HL2's own receive shipped. **Done-when:** two bands decode for an hour with every loss counter at zero or explained, and a CQ on the second band completes a QSO there (field); or the finding that one P4 carries one HL2 band. **After:** row [q9my](hl2-support.md#q9my), which measures what sharing the N2ADR's filter costs.
* **A QMX and an HL2 at once:** channels from each, a reply transmitted by the radio whose channel heard it; one backend per radio in place of one selected backend (epic [knpp](hl2-support.md), **Open**).

## Open

* **The work unit:** LDPC iterations plus fixed costs, or a coarser unit (one candidate) if the iteration count is not exposed without patching `ft8_lib` ([ft8_lib.md](../architecture/ft8_lib.md): our changes there stay Karlis-shaped) ([vnz7](#vnz7)).
* **The margin** between budget and deadline: fixed, or measured per board from the backstop's record ([vnz7](#vnz7)).
* **Subtraction with two workers:** (a) subtract at the end of each pass, a pass's candidates decoding in parallel; or (b) decode in parallel but commit and subtract in candidate order, discarding a decode a prior subtraction would have changed. Today each decode subtracts before the next candidate is tried, so the order changes the output. Scored on the recall harness ([vnz7](#vnz7); [RFC 0006](../rfcs/0006-decode-performance.md) §4.2).
* **What the operator sees** when admission refuses a channel, and when a channel ran only its minimum.
* **The QMX's receive dial with FT8 and FT4** ([an8x](#an8x)): FT8's dial, putting FT4's channel 19.5 kHz above the LO on 20 m, near the window's top, or between the two, moving FT8's channel off today's 13,500 Hz. **FT4 decodes well at that edge:** `jt9 -5` on the 2026-11-02 QMX corpus's 20 m I/Q, the channel 19.5 kHz above the LO, found 980 FT4 decodes in 120 periods (8.2 a period, every period with some, down to −21 dB), with no thinning toward the window's top (decodes per 500 Hz of audio: 47, 179, 264, 250, 202, 38, the usual activity shape); no FT4 away from the edge to compare with, so it shows FT4 works there, not that it loses nothing ([the corpus's README](../../tools/decode_bench/corpus/qmx-2026-11-02/README.md), **FT4 at the window's top**). It leans toward FT8's dial.
* **Transmitting off the receive dial on a QMX** ([grqg](#grqg)): a far `TA` tone, if the radio takes one, or a retune for the transmission and back; settled on the bench.
* **A CQ in progress** on one channel when we reply on another: pause it, cancel it, or keep it ([grqg](#grqg)).
* **The transmit band table:** the full allocations for the operator's licence class, or only the segments around each mode's calling frequency ([grqg](#grqg)).

## Facts

Read from the working tree at `ef4e271`.

* **One channel, one protocol.** `g_protocol` is set once at boot (`poe_p4/main/main.cpp:30`) and read throughout capture and decode. The decode's working sets are file and function statics: `s_dec` (`decode_pipeline.cpp:138`), the candidate list, `seen_hz` and the retry state, the waterfall in `components/ft8_lib/common/monitor.c`, and one `freq_osr` in `decode_tx_state`. The work is already in small steps: each search pass calls `ftx_find_candidates()`, then `ftx_decode_candidate()` once per candidate, subtraction inside that loop.
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
* **Several P4s joined:** whole channels placed per board would scale, but worth designing only once one P4's capacity is measured; [vnz7](#vnz7)'s numbers say what one carries.
* **Other modes now:** WSPR (slotted at 2 minutes) or CW and RTTY (streaming) fit the same declarations, but are row [I6](../ROADMAP.md#I6)'s; each needs its own MIT implementation, since the obvious reference decoders (JS8Call's tables, `wsprd`) are GPL.
