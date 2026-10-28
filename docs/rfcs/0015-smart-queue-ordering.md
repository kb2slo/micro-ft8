# RFC 0015: Smart queue ordering — spend each transmission where it can be answered

* **Status:** Draft 2026-10-27. Decided in discussion on 2026-10-27: §9. The Ask: §8.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** the order in which queued contacts transmit; holding a contact while its station works someone else; the opening call's tries; moving entries by hand; how the station picks among callers to our CQ; a reconfigurable priority ladder; POTA and SOTA priority, park-to-park included.
* **Does not cover:** adding stations to the queue that neither the operator tapped nor called us, which is auto-reply ([RFC 0009](0009-operate-station-surface.md) §5, row r2bu); working several stations at once, as WSJT-X's Fox mode and MSHV's multi-answer mode do; DXCC, grid or park "needed" priority (§5.8).
* **Rows:** none yet; a row for the maintainer to place.

---

## 1. Goal

Every transmission the queue spends on a station that cannot answer is a slot not spent on one that can. Order the queue, and hold entries back, so that transmissions go where an answer is likely, without adding a station to the queue that a person did not choose.

1. **Do not call a station that is working someone else.** Hold the entry and call the moment that station is free.
2. **Stop an opening call sooner.** Answers to repeated opening calls fall off fast (§3).
3. **Let the operator reorder the queue** by hand: pin, up and down, next.
4. **Choose among callers to our CQ, and among our own calls, by a rule the operator sets** in one place: farthest, strongest, POTA or SOTA, and so on.

**Constraints:**
* What a queued entry transmits stays bounded by its tries ([autoseq.md](../architecture/autoseq.md), **Tries**). A held entry transmits nothing.
* WSJT-X is the on-air standard ([AGENTS.md](../../AGENTS.md), **Operating notes**). Nothing here changes what a message says; it changes when we send it and to whom first. Where that differs from WSJT-X it is a recorded decision (§9).
* The ordering runs on the main board, in `components/autoseq/`, and is `host_mock`-tested. The web app shows it and sends the operator's moves; it does not order anything itself.

## 2. What exists

Facts are from the working tree at `5f98c19`.

* **Only `queue[0]` transmits.** The active zone is sorted by `compare_ctx()` (`components/autoseq/autoseq.cpp:1426`): IDLE first, then by state, further along first (SIGNOFF > ROGERS > ROGER_REPORT > REPORT > REPLYING > CALLING, so the CQ is last), then within one state fewer tries sent first ([autoseq.md](../architecture/autoseq.md), **The queue**). Nothing in the sort looks at the station's activity, its distance, or the entry's slot parity.
* **Entries come from a tap or from a decode addressed to us** ([autoseq.md](../architecture/autoseq.md), **Where entries come from**). Every station that calls us gets an entry without a tap; a caller answering our CQ with a grid lands in REPORT. **Differs from WSJT-X**, which with Auto Seq on turns Enable Tx off at the end of each QSO and "will not make consecutive, fully automated QSOs" (its user guide, `doc/user_guide/en/make-qso.adoc` at `967c85a`).
* **The engine sees only decodes addressed to us.** `decode_pipeline_apply()` passes `autoseq_on_decodes()` the decodes whose first field is our call. Whether a queued station is working someone else is not visible to it.
* **The web app already sees it.** A decode naming a queued station is marked **CQ again**, **Working** someone else, **Done with** someone else, or **Called by** someone else, and the queue row repeats the newest such phrase with its age (`queueInvolvement()`, `queueLastHeard()`; [webapp.md](../architecture/webapp.md), **Decode rows and tap to reply**). It changes nothing the station sends.
* **A per-call table already exists on the main board:** the heard-grid table (`components/decode_pipeline/heard_grid.h`), 128 calls kept 4 hours from when each was last heard, on uptime, filled from every decode by `decode_pipeline_apply()` ([autoseq.md](../architecture/autoseq.md), **Classifying what DX sent**). §5.2 and §5.7 add to it.
* **Tries:** one Station key `tries` (1–20, default 6) bounds each QSO message; an entry can have its own count, 1–20, from its row ([autoseq.md](../architecture/autoseq.md), **Tries**).
* **`autoseq_rotate_same_parity()`** moves `queue[0]` behind the other front entries of its parity. Nothing calls it.

## 3. Evidence

**Source:** the maintainer's WSJT-X `ALL.TXT`, 2026-05-28 to 2026-08-06: 78,828 lines, 75,525 decodes and 3,303 of our own transmissions, mostly 20 m and 40 m FT8. It is the maintainer's own log and is not committed. Every number below is from it; callsigns are left out. The scripts are listed in Ask 6.

**Our transmissions to a station, by what that station was doing in the four slots before** (an answer counts when it is decoded within two slots):

| The station was | Our TX | Answered |
|---|---|---|
| Calling CQ | 731 | 60 (8.2%) |
| Calling us | 688 | 445 (64.7%) |
| Not heard in the four slots | 229 | 10 (4.4%) |
| **Working someone else** | **169** | **1 (0.6%)** |
| Just sent RR73, RRR or 73 to someone else | 90 | 5 (5.6%) |

* **Opening calls to a busy station:** 130, and none was answered within two slots; 7 were answered within five minutes, after the station finished.
* **Answers fall off with each repeated opening call.** Of 449 runs of opening calls to one station: call 1 answered 9.1%, call 2 3.9%, call 3 4.7%, call 4 1.4%, and calls 5 and later 0 of 55.
* **After an unanswered opening call** the station was next heard calling CQ again 34% of the time (it did not decode us), working someone else 19%, and not heard 47%.
* **A station that sends RR73, RRR or 73 to someone else** is next heard a median 2 slots later (75th percentile 4). Next, it answers a station already calling it (2,414 times) more often than it calls CQ (1,822). The slot right after its closing message is when to call.
* **Callers to our CQ give up fast.** Callers we never answered called once (37), twice (23), three times (6) or more (8). Of the 62 callers passed over in a pileup, 28 (45%) were worked within ten minutes.
* **A caller's SNR barely predicts whether the QSO completes.** Of 156 callers we sent a report to, 60–80% reached an R-report or RR73 in every 4 dB band from −24 to +11 dB. Ordering callers by something other than strength costs little.
* **POTA and SOTA are common:** 1,007 `CQ POTA` and 148 `CQ SOTA` of about 17,400 CQs.

**A replay simulation.** The real band timeline drives everything but our station. An operator taps CQing stations heard in the previous slot into a queue of five; the rule under test picks what transmits each slot. A station answers with the probabilities above, never in a slot where it really answered someone else, and its answer is heard only in a slot we are not transmitting in. 25.5 hours, 20 runs each, an operator who taps in 30% of slots that offer a CQ:

| Rule | QSOs | TX | TX per QSO |
|---|---|---|---|
| Today's ordering, 6 tries | 66.5 | 2,297 | 34.5 |
| Hold busy stations, tries frozen | 67.1 | 2,239 | 33.4 |
| Today's ordering, 4 opening tries | **70.2** | 2,045 | 29.1 |
| Today's ordering, 3 opening tries | 66.5 | 1,778 | 26.7 |
| Also hold stations not heard for 4 slots | 64.1 | 1,594 | 24.8 |
| Also set aside a station after two CQs over our calls | 54.6 | 1,168 | 21.4 |

With an operator who taps twice as often, holding busy stations gave 89.4 QSOs against 82.1 (+9%).

**Limits of this evidence.** The answer probabilities are measured on the same log the simulation replays, so the table compares rules; it does not forecast QSO rates. The low answer rates are one station's, probably at low power. The log covers opening calls well and mid-QSO repeats hardly at all. FT8 only.

**Mixed parity is rare in practice.** `compare_ctx()` does not look at parity, so two active entries on opposite periods would alternate and the station would transmit in every slot, missing each station's answer in the slot it transmits to the other. In the simulation it never happened: while we transmit on one period we hear only the other, and the stations heard there are the ones we would call on our period. A tap on an older decode row can still cause it, which §5.9 settles with a test.

## 4. Comparable programs

Read from source on 2026-10-27; versions are the commits read.

* **WSJT-X 3.0** (`WSJTX/wsjtx` at `967c85a`). One QSO at a time, no queue.
  - **Wait and Call:** once the station in DX Call is decoded again sending CQ, RR73, RRR or 73, it is called automatically up to three times. **Wait and Reply** resumes a QSO when that station calls us later. **Wait and Pounce** answers a CQ that meets a chosen rule (`make-qso.adoc`, **Wait Features**; `widgets/mainwindow.cpp` around line 6451).
  - **Reply to our CQ:** CQ: First, CQ: Max Dist, CQ: Max dB, CQ: Min dB; the last three skip dupes from its own log.
  - **Fox mode** sorts callers by Random, Call, Grid, SNR, Distance, Age, Continent or a user value (`sortHoundCalls()`, `mainwindow.cpp:14370`); an Alt double-click puts a caller at the top.
* **JTDX** (`jtdx-project/jtdx` at `2a0e2be`). One priority ladder scores every caller (`displaytext.cpp`): new CQ zone, then new ITU zone, new DXCC, new DXCC on this band, new grid, new grid on this band, each one step higher for a LoTW user. Ties go by SNR, by "already sent a report", or by distance (`qsohistory.cpp`, `autoseq()`). "Halt TX if operator I called answered to other operator", and counters that stop after N unanswered calls.
* **MSHV** (`LZ2HV/MSHV`). A multi-answer mode with Queue and Now tabs, a queue limit, a cap per caller in minutes or periods, sorting off, by distance or by SNR, and adding a call by double-click (`HvTxW/hvmultianswermodw.cpp`).
* **FT8CN** (`N0BOY/FT8CN`). A watch list whose stations are called when they CQ; a call that answers us outranks the watch list; an RR73 that is not acknowledged is dropped once the partner is heard calling someone else (`FT8TransmitSignal.java`).
* **QMX panadapter** (`SteffenLav/qmx-panadapter` at `40daf5c`, the M5Stack Tab5 station).
  - An opt-in auto-answer of others' CQs ranks by strongest, weakest or farthest (`main/ft8_robot.c`, `rank_score()`), and holds until both periods have been listened to.
  - A pileup list of callers not yet worked, acted on only by a tap (`main/ft8_pileup.h`).
  - A grey list: a station is set aside after two timed-out attempts, or at once by the operator's Skip (`main/ft8_greylist.h`).
  - An inventory of every path that can key the radio, checked by a script, written after a user's unit answered four JS8 callers on its own (`docs/unattended-tx-inventory.md`).

## 5. Design

### 5.1 The order

The active zone sorts by these keys, in order. The first two are today's rules; the rest are new.

1. **Stage**, as today: IDLE first, then further along first, the CQ last.
2. **Held entries last within their stage** (§5.2). A held entry is not transmitted; it sorts behind the others so `queue[0]` is an entry that can go.
3. **A one-shot Next** (§5.4) goes ahead of everything but the transmission on the air, for one transmission.
4. **Pins, within REPLYING only** (§5.4). A pin outranks our other opening calls, never a station calling us and never a QSO in progress: the data says a station calling us is answered 65% of the time and leaves within a call or two, an opening call 9% at best.
5. **Manual moves** within the stage (§5.4).
6. **The ladder** (§5.6): the operator's reorderable list of rungs, the first rung that tells two entries apart deciding. "Fewest tries" is one rung, and the default ladder keeps today's round-robin behind Farthest.

`sort_and_clean()` still runs only where it runs today (after decodes, a touch, a reactivation), plus after an operator move. Every key is a field on `QsoContext`, so one comparison function stays the whole rule and `host_mock` can drive every case.

### 5.2 Holding an entry while its station works someone else

* **Busy** means: within the last 4 slots, the station was decoded sending a message that continues a QSO with another call (a grid, a report, an R-report, or a Field Day exchange, addressed to a call that is not ours and not CQ).
* **Free** means: its newest such decode is a CQ (with any modifier), or an RR73, RRR or 73 to someone else, or a message to us.
* **A held entry transmits nothing and spends no tries.** Its tries resume where they were when it is released.
* **Release** is the first decode that makes the station free. A release from RR73, RRR or 73 lands in the slot before ours, so the entry goes out in the next slot; §3 says that is when to call.
* **A hold ends after 5 minutes.** The entry then ends as if its last try were used ([autoseq.md](../architecture/autoseq.md), **The slot cycle**): REPLYING is evicted, REPORT and later are parked, so a later message from that station still finds its entry.
* **Which entries:** every QSO stage up to ROGER_REPORT. ROGERS and SIGNOFF already send once and park, and the CQ is never held.
* **The data:** a new per-call record of the station's last activity: the slot, the kind of message, and whom it was to. It is filled from every decode, as the heard-grid table is, and read by the sort. One table or a sibling of `heard_grid.h` is an implementation choice, not settled here.
* **What the operator sees:** the row says the entry is held and why ("Working W1XYZ · waiting"), from the same `QUEUE_ENTRY` stream with one new flag. The phone's own cue (§2) stays as it is.
* **Matches WSJT-X's Wait and Call** in effect, without its three-call limit: here the entry's own tries bound the calls after release.

### 5.3 The opening call's tries

* **An opening call (REPLYING) gets at most 4 tries:** `min(tries, AUTOSEQ_OPENING_TRIES)` with `AUTOSEQ_OPENING_TRIES` = 4. Every later stage keeps the Station key `tries`.
* **An entry's own count overrides it,** as today: the row's stepper sets the opening call's tries too.
* **No new setting.** One Tries on the Station sheet, as decided on 2026-10-14 ([autoseq.md](../architecture/autoseq.md), **Tries**); the opening limit is a constant, named once and pinned by a host test.
* **The data:** calls 5 and later were never answered (§3), and in the simulation 4 tries gave the most QSOs with fewer transmissions: the freed place goes to the next tap.

### 5.4 Moving entries by hand

* **Pin** (a toggle). A pinned REPLYING entry sorts ahead of the other REPLYING entries; several pins keep the order they were pinned in. A pin ends when the entry leaves REPLYING, since the station has answered and stage now orders it.
* **Up and down** swap an entry with its neighbour within the same stage. A move is kept as a manual rank that the ladder does not override; it is cleared when the entry changes stage. A move across a stage boundary is refused, and the button is not offered.
* **Next** puts the entry first for its next transmission, behind a transmission already on the air, as a touch does; it is cleared after that transmission. It is the one operator action that may outrank a QSO in progress, and it is one transmission, not a standing order.
* **Port A:** one `QUEUE_ORDER` message carrying `entry_id` and the action (pin, unpin, up, down, next), addressed by `entry_id` as `QUEUE_CANCEL` and `QUEUE_TRIES` are. Refusals carry a reason.
* **Web app:** the actions go in the sheet a row's tries count already opens (`#opTriesEdit`), which becomes the entry's sheet, rather than on the row: Operate shows about two queue rows, and they are rebuilt on every poll ([webapp.md](../architecture/webapp.md), **Tries on a queue row**). A pinned row shows a pin mark.

### 5.5 Callers to our CQ

* **Every caller still gets an entry without a tap,** as today (§2), and that is now a recorded decision (§9), not an inherited behaviour.
* **The ladder picks among them.** Callers that answered in the same slot are at the same stage with no tries sent, so the ladder alone decides who gets the first report. That covers WSJT-X's CQ: First, Max Dist and Max dB as rungs (§5.6).
* **A caller is never held for being busy until we have sent it something;** a station calling us is free by definition (§5.2).

### 5.6 The ladder

* **A Station key, `queue_order`:** an ordered list of rung names, for example `far,tries,first`. It is stored on the main board like `tries`, so every phone and a reload see the same order, and a change applies at the next sort.
* **Rungs in phase 1:**
  - **Farthest:** distance from the Station grid to the station's grid, from the QSO or the heard-grid table; no grid sorts last.
  - **Strongest:** the SNR of the station's newest decode.
  - **Fewest tries:** today's rule 4.
  - **First heard:** the order entries were created; always the final tie-break, so the order is total.
* **The default ladder:** Farthest, Fewest tries, First heard. Strongest is not in the default, because SNR barely predicts completion (§3).
* **Web app:** a "Queue order" list on the Station sheet with drag handles, one row per rung. The sheet is where set-and-forget station preferences live; each control has one home ([webapp.md](../architecture/webapp.md), **The Station sheet**).

### 5.7 POTA and SOTA (phase 2)

* **A heard-activation record per call:** a station decoded sending `CQ POTA` is marked a POTA activator, `CQ SOTA` a SOTA activator. It is kept as the heard-grid table keeps a grid (§2), for hours, since an activation lasts that long. Live spots the phone already fetches are a later, second source; they need a path from the sidekick to the main board.
* **Rungs:** **POTA activator** and **SOTA activator**, ordinary rungs on the ladder, so a hunter at home can put them first.
* **Park to park:** when our station is activating (CQ type POTA or SOTA, or an activation set up, [RFC 0008](0008-activator-mode.md)), a station known to be activating gets the same priority whoever called whom. The P2P rung covers both our calls and stations calling us.
* **Whether the ladder follows the CQ type,** as Field Day does, so an activation has its own order, is a phase 2 question.

### 5.8 Deferred: "needed" (DXCC, grid, park)

Ranking by a new DXCC entity, a new grid or a park not yet worked needs a worked-before lookup the main board cannot do well from flat `.adi` files. It waits for the log database (row I32), as the web app's New filter does ([webapp.md](../architecture/webapp.md), **Decode sort and filter**). Nothing in this RFC decides how "needed" works; when I32 lands, needed rungs join the ladder as any rung does.

### 5.9 Mixed parity: test first

A `host_mock` scenario puts two REPLYING entries on opposite parities and checks what transmits in consecutive slots. If they alternate (as `compare_ctx()` suggests), the fix ships in slice Q1: the head's parity holds the period, and an entry on the other period waits until no entry on the head's period can transmit. If they do not alternate, the scenario stays as the proof and nothing changes.

### 5.10 A station that calls CQ over our calls

**Nothing special happens; the opening limit bounds it** (§5.3). A CQ is a free station, so it is not held, and it gets at most 4 calls. The row already says "CQ again" (§2), and × ends the entry. Setting such a station aside is rejected (§6). Moving our TX offset after a CQ over us, in case we land on someone else's signal, is an idea only: §3 cannot test it, since that log held its offset. It also has to fit row B88 (hold the TX offset for a QSO), which concerns a QSO already started.

## 6. Rejected alternatives

* **Holding a station not heard for some slots.** It saves 10–30% of transmissions for about 5% fewer QSOs (§3, at 4 to 12 slots), and 4 opening tries already bound calls to a station that has gone. Decided 2026-10-27.
* **Setting a station aside after two CQs over our calls** (the panadapter's grey list, JTDX's counter). In the simulation (§3 settings, busy hold on) it cut QSOs from 66.2 to 58.7. It exists for programs that choose stations on their own; here every opening call is a tap.
* **A pin above every entry.** It would trade a station calling us (65% answered, and gone within a call or two) for one that has not answered (9% at best). Next covers "this one now" for one transmission.
* **Strongest first by default.** SNR barely predicts completion (§3).
* **A separate opening-tries setting.** One Tries setting was decided on 2026-10-14; a constant does the job.
* **The ladder on the queue.** Operate shows about two queue rows, rebuilt every poll; the ladder is a station preference.
* **Several QSOs at once** (Fox, MSHV multi-answer). Out of scope: it needs multi-tone TX and a different on-air protocol.

## 7. Slices

* **Q1: busy hold.** The activity record, the held flag, release and the 5-minute end; the `QUEUE_ENTRY` flag and the row's "waiting" text; the mixed-parity scenario (§5.9). **Done-when:** `host_mock` scenarios hold an entry while its station works another call, release it on RR73 and transmit in the next slot, end a hold at 5 minutes, and never charge a try while held; a held entry is seen waiting on Operate and goes out after its station's RR73 (field).
* **Q2: opening tries.** `AUTOSEQ_OPENING_TRIES`. **Done-when:** a host test shows REPLYING ends after 4 with `tries` at 6, after 2 with `tries` at 2, and after the entry's own count when one is set.
* **Q3: manual order.** `QUEUE_ORDER`, the entry's sheet, the pin mark. **Done-when:** host tests for pin, up, down and next, including refusals across stages; on the phone, each action moves the row and changes what transmits next (field).
* **Q4: the ladder and callers to our CQ.** `queue_order`, the phase 1 rungs, the Station sheet's Queue order. **Done-when:** host tests order two simultaneous callers by each rung; the Station sheet's order survives a reload and applies to the next sort.
* **Q5 (phase 2): POTA and SOTA.** The heard-activation record, its rungs, park to park. **Done-when:** host tests mark a call from `CQ POTA` and order by the rungs, and P2P ranks the same whoever called.
* **Later:** live spots as a second source for Q5; needed rungs after I32.

Each slice graduates what it builds into [autoseq.md](../architecture/autoseq.md) and [webapp.md](../architecture/webapp.md).

## 8. Open questions — the Ask

1. **Accept the order** (§5.1): stage, held last, Next, pins in REPLYING only, manual moves, then the ladder?
2. **The busy hold's numbers:** busy if a QSO message to another call was heard within 4 slots, and a hold ends at 5 minutes?
3. **Held at which stages:** up to ROGER_REPORT, as proposed, or opening calls only? Mid-QSO, the log in §3 has little to say.
4. **The default ladder:** Farthest, Fewest tries, First heard?
5. **The phase split:** Q1–Q4 as phase 1, POTA and SOTA as phase 2?
6. **The scripts:** commit the simulation and the log analysis under `tools/` (they read any WSJT-X `ALL.TXT`; the log itself stays private), or keep them out of the repo?

## 9. Decisions

* **2026-10-27, in discussion (operator):**
  - Order only what is already queued; never add a station on our own (auto-reply stays row r2bu). Picking among callers to our CQ is in scope.
  - Hold a station that is working someone else, with tries frozen.
  - No hold for a station that has gone unheard; the opening limit covers it.
  - An opening call gets 4 tries.
  - A pin outranks only our other unanswered calls, never a station calling us or a QSO in progress. Manual moves are pin, up and down, and next.
  - Callers to our CQ get an entry without a tap, as today, and the RFC records it. **Differs from WSJT-X**, which stops after each QSO.
  - One ladder, easy to reconfigure, set on the Station sheet and stored on the main board.
  - POTA and SOTA are rungs used at home too, possibly phase 2. Park to park ranks the same whoever initiated, and activators are known from a per-call cache like the heard-grid table.
  - DXCC, grid and park "needed" are deferred to the log database (I32).
  - A station that calls CQ over our calls gets no special treatment beyond the opening limit.
