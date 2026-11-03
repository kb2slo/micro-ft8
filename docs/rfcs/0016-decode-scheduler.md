# RFC 0016: A decode scheduler — one decoder for 1–N channels

* **Status:** Accepted 2026-11-03 (drafted 2026-11-02). Decided: §8. Still open, each settled by a dated decision when its slice needs it: §7 items 2–5.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** decoding several channels at once on one P4 (FT8 and FT4 from one QMX in IQ mode; up to two HL2 bands per P4) within the time each channel has, on both cores, and across several P4s by placing whole channels on boards; how decode degrades as channels are added; how that stays deterministic under test.
* **Does not cover:** the I/Q front end ([RFC 0014](0014-hl2-two-p4.md) §3.4, row y9bt); the HL2 backend, and the helper protocol between P4s (RFC 0014 §3.5, S4); subtracting decodes from the audio (row kkap), AP decoding (row qauk) and t4/f2 (row n2e7), which this RFC gives a place but does not design; modes other than FT8 and FT4 (row I6), which it must not rule out.
* **Rows:** [vnz7](../ROADMAP.md) builds S1–S3.

---

## 1. Goal

**Decoding more channels must degrade, never overrun** (operator, 2026-11-02). One channel gets the deepest decode the slot allows; each channel added takes some depth from the others, in order of what is worth least, and no slot's decode runs past its deadline.

**Constraints:**

* **The QMX station decodes exactly as today** while it has one FT8 channel and time to spare: the recall floors and the `tx_e2e` goldens hold.
* **Deterministic under test.** The host replays any mix of channels and gets the P4's output byte for byte; CI does not depend on the host's speed.
* **No mode's timing is built in.** The vision is several digital modes (row I6); this RFC builds FT8 and FT4 only, but nothing in the scheduler assumes a 15 s slot or even/odd periods.
* **No board count is built in.** The vision is several P4s joined to decode more channels (operator, 2026-11-03); this RFC builds one, RFC 0014's S4 the second, and nothing here assumes either count.

## 2. What exists

### 2.1 In the code

* **One channel, one protocol.** `g_protocol` is set once at boot (`poe_p4/main/main.cpp:30`) and read throughout capture and decode (`components/ft8_audio_pipeline/ft8_audio_pipeline.cpp:111`, `:263`; `components/decode_pipeline/decode_pipeline.cpp`). The decode's working sets are file and function statics: `s_dec` (`decode_pipeline.cpp:138`), the candidate list, `seen_hz` and the retry state (`:334`–`:336`), the waterfall in `components/ft8_lib/common/monitor.c` (`:19`–`:23`, `:72`), and one `freq_osr` in `decode_tx_state`.
* **Decode runs inside the capture task,** pinned to core 1 (`poe_p4/main/main.cpp:93`), and starts after 79 blocks, 12.64 s into the slot, leaving a 2.36 s guard before the next boundary ([decode.md](../architecture/decode.md)). An ingest task fills a 2.7 s ring meanwhile.
* **The work is already in small steps.** Each search pass calls `ftx_find_candidates()` and then `ftx_decode_candidate()` once per candidate (`decode_pipeline.cpp:374`–`:413`); subtraction runs inside that loop, after each decode.
* **Our TX slot is skipped** by slot index (`tx_slot_idx`, `ft8_audio_pipeline.cpp:253`–`:283`).

### 2.2 Measured

* **Cost.** A busy 20 m slot decodes in about 1.75 s of one core; a quiet one in 2.15–2.3 s, because failing candidates run LDPC to its limit; on 15 m 24 of 207 slots overran the guard (2026-10-31, [decode.md](../architecture/decode.md); row vnz7). During decode one core reads 2% busy, the other 100%.
* **Recall plateaus on crowded bands** ([corpus `corpus-2026-11-02`](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-02)): at about 25 jt9 messages a slot we decode about 67% of them; at 56, about 47%, levelling near 27 decodes a slot. On `20m_busy`'s ten most crowded slots we decode 90% of stations with no stronger one within 60 Hz and 6% of those within 30 Hz of one. More candidates, a third subtraction pass, or more LDPC iterations each move that by under 2%; t4/f2 adds 2–4% on every band measured.

So depth beyond today's first two passes buys little until subtraction improves (row kkap), and a second channel's first pass is worth far more than either channel's third.

## 3. Design

### 3.1 A channel

A channel is a source of real audio at the pipeline's rate (a QMX's demodulated audio, or one output of the I/Q front end fed by a local radio or by I/Q forwarded over Ethernet from another board), a dial frequency, and a **mode**. All of today's single-instance state (§2.1) becomes per channel; the FFT plan and window are shared. The mode is a value the channel carries, not a global.

### 3.2 A mode declares its timing and its work

The scheduler never asks which mode a channel runs. Each mode declares:

* **Timing:** *slotted* (period, the trigger point in the slot, the deadline), or *streaming* (process each block; keep up). FT8 is slotted at 15 s, FT4 at 7.5 s. The trigger point is per mode, which is where a later trigger ([RFC 0006](0006-decode-performance.md) §4.3) lives if it is ever taken.
* **Work in steps,** each with a cost: for FT8 and FT4, one search pass, and one candidate decoded.
* **Its minimum:** the work without which the slot is worthless. For FT8 and FT4, the first search pass and its candidates.
* **When it is deaf:** while we transmit. Today's TX-slot skip becomes a property of each channel.

### 3.3 Order: earliest deadline first, then value

Pending work is ordered by:

1. **The QSO partner first:** the channel of the contact in progress, and on it the candidates near the partner's tone, so a reply is decided in time for the next slot ([RFC 0014](0014-hl2-two-p4.md) §3.5).
2. **Every channel's minimum, earliest deadline first.** With per-channel deadlines (an FT4 deadline falls twice in each FT8 slot) this is the textbook policy.
3. **Then deeper work, round-robin across channels, earliest deadline first:** further passes, and later what rows kkap, n2e7 and qauk add, each placed by its measured gain per cost.

### 3.4 The budget is counted, not timed

* **Work is counted in units the host can replay:** LDPC iterations run, plus a fixed cost per search pass and per candidate considered. Each unit's P4 cost is measured once on the bench (`decode_bench`) and kept in a table.
* **A slot's budget** is its time to the deadline, less a margin, converted to units by that table. The scheduler's decisions depend only on counts, so the host, given the same audio and the same budget, makes the same decisions and decodes the same messages.
* **The wall clock is a backstop, never the policy.** If a slot reaches its deadline before its budget is spent, decode stops, the slot is logged as overrun, and the cost table is wrong and gets re-measured.

### 3.5 Admission and placement, instead of starvation

Adding a channel is refused, with a reason the web app shows, when the sum of every admitted channel's minimum (at its worst case: the full candidate list) would not fit each channel's budget. So no channel goes dark at runtime: what was admitted can always run its minimum, and only depth beyond it is shared.

With several boards, admission is **placement**: a new channel goes to the board whose budget fits its minimum, or is refused when none does. The QSO partner's channel is placed on the station board, so the reply decision never waits on the network.

### 3.6 Cores and boards

**Within a board, units of work; across boards, whole channels.** A network round trip costs more than a unit, and a scheduler whose order depended on the network could not be replayed, so units never cross boards.

* **Across boards.** Each board runs this scheduler on the channels placed on it. The station board holds the station's state; a helper holds none and returns its decodes by the slot's deadline, a late one dropped and counted ([RFC 0014](0014-hl2-two-p4.md) §3.5). With an HL2 the boards stop paying at its four receivers; more channels then need more sources, a different station design.
* **Within a board.** Work units go on one queue that a worker on each core draws from. Core 0 also carries USB or Ethernet and, with IQ, the front end; its share of the budget is what remains after those, measured.

**Open: subtraction and determinism.** Today each decode subtracts before the next candidate is tried (`decode_pipeline.cpp:374`–`:413`), so the order of decodes changes the output. With two workers either (a) subtraction happens at the end of each pass, and a pass's candidates decode in parallel, or (b) candidates decode in parallel but commit and subtract in candidate order, discarding a decode a prior subtraction would have changed. The recall harness scores both before one is chosen (row vnz7's open question; [RFC 0006](0006-decode-performance.md) §4.2).

### 3.7 Visibility

Each slot's SLOT line reports, per channel, the board it runs on, the work done and the depth reached (minimum only, which pass), and whether the backstop fired. A band that ran only its minimum is visible to the operator and in the logs, as RFC 0014 §3.11 asks of every loss.

### 3.8 Beyond FT8 and FT4

* **Other modes fit the same three declarations.** WSPR is slotted at 2 minutes with a deadline minutes away, so its work fills whatever the short slots leave; CW and RTTY would be streaming. Building any of them is row I6's, not this RFC's.
* **Licensing.** The obvious next modes have GPL reference decoders: JS8's tables exist only in JS8Call, and `wsprd` is WSJT-X's. A clean boundary between scheduler and mode does not change linking: each mode needs its own MIT implementation, written from its specification, as FT8 is.

## 4. Alternatives

* **Fixed tiers by channel count** (one channel deep, four channels shallow). Simple, but cost depends on the band more than on the count (a quiet band costs more than a busy one, §2.2), so a tier is too cautious on a busy evening and still overruns on a quiet one.
* **A wall-clock deadline as the policy.** Adapts to anything, but the output then depends on CPU speed: the host cannot replay it, and CI's recall floors drift.
* **One core per channel.** Deterministic without §3.6's open question, but one channel then uses half the P4, and two channels cannot borrow from each other.
* **Units of work shared across boards.** Balances load best, but each unit would wait on a network round trip that costs more than the unit, and the order would depend on the network, so the host could not replay it.
* **One decoder per mode, each with its own task.** FreeRTOS priorities would then decide which band goes dark, invisibly.

## 5. Slices

Each slice states its verifiability category from [AGENTS.md](../../AGENTS.md).

1. **S1: per-channel state.** §3.1's refactor, with one FT8 channel; `g_protocol` out of the decode path.
   - Verification: the recall floors and `tx_e2e` goldens unchanged, byte for byte (category 1); a QMX decodes and transmits as before (category 2).
2. **S2: the budget and the order, one core.** §3.2–3.5 and §3.7: work counted, earliest deadline first, the minimum, the backstop, the SLOT line.
   - Verification: host tests replay a mix of channels to identical output, and an ample budget reproduces S1's output exactly (category 1); a quiet band no longer overruns, because decode stops at its budget (category 2).
   - **Done-when:** an hour on a quiet band with no slot past the guard; host recall within the CI floors.
3. **S3: two cores.** §3.6, with the subtraction question decided on the recall harness.
   - **Done-when:** row vnz7's: busy slots well under 1,747 ms, recall within the floors, no overrun.
4. **S4: a second channel.** FT4 beside FT8 from one QMX in IQ mode (after row y9bt's firmware stage), or a second HL2 band (RFC 0014 S3), whichever comes first.
   - **Done-when:** both channels decode for an hour with every slot inside its deadline and each channel's depth logged (field).

## 6. What changes elsewhere on acceptance

* [ROADMAP.md](../ROADMAP.md): row vnz7's Done-when becomes S2–S3 here (§8), with S1 ahead of it; this RFC's own row drops.
* [RFC 0014](0014-hl2-two-p4.md) §3.5 points here for per-channel state and the CPU budget instead of growing.
* [decode.md](../architecture/decode.md) gains the scheduler as each slice ships (graduation).

## 7. Open questions — the Ask

1. **Accept the direction** (§3) and the slice order (§5)?
2. **The work unit** (§3.4): LDPC iterations plus fixed costs, or a coarser unit (one candidate) if the iteration count is not exposed without patching `ft8_lib` ([ft8_lib.md](../architecture/ft8_lib.md): our changes there stay Karlis-shaped)?
3. **The margin** between budget and deadline: fixed, or measured per board from the backstop's record?
4. **Subtraction with two workers** (§3.6): (a) or (b), on the harness's numbers.
5. **What the operator sees** when admission refuses a channel, and when a band ran only its minimum.

## 8. Decisions

* **2026-11-03:** accepted: the direction (§3) and the slice order (§5); §7's items 2–5 stay open (operator).
* **2026-11-03:** several P4s may be joined to decode more channels; whole channels are placed on boards, units of work never cross a board, and the QSO partner's channel stays on the station board (operator).
* **2026-11-02:** each channel has its own deadline, and work runs earliest deadline first, so modes that are not 15 s even/odd fit (operator).
* **2026-11-02:** admission control on each mode's declared minimum replaces "always a first pass": a channel the budget cannot carry is refused when it is added (operator).
* **2026-11-02:** row vnz7's Done-when becomes this RFC's S2–S3 (operator).
* **2026-11-02:** several digital modes are the vision; the scheduler assumes none of their timings and this RFC builds FT8 and FT4 only (operator).
