# Smart queue ordering (epic qxy5)

Spend each transmission where it can be answered: order the queue, and hold entries back, so transmissions go where an answer is likely, without adding a station to the queue that a person did not choose. Rows: [jqpm](#jqpm) ([ROADMAP.md](../ROADMAP.md)). How the queue works now: [autoseq.md](../architecture/autoseq.md), **The queue**.

## Goal

1. **Do not call a station that is working someone else.** Hold the entry and call the moment that station is free.
2. **Stop an opening call sooner.** Answers to repeated opening calls fall off fast (**Evidence**).
3. **Let the operator reorder the queue** by hand: pin, up and down, Next.
4. **Choose among callers to our CQ, and among our own calls, by one rule the operator sets:** farthest, strongest, POTA or SOTA, and so on.

## Constraints

* **Order only what is already queued; never add a station on our own.** Auto-reply stays row [r2bu](ideas.md#r2bu). Picking among callers to our CQ is in scope.
* **What an entry transmits stays bounded by its tries** ([autoseq.md](../architecture/autoseq.md), **Tries**). A held entry transmits nothing.
* **WSJT-X is the on-air standard.** Nothing here changes what a message says, only when we send it and to whom first.
* **The ordering runs on the main board,** in `components/autoseq/`, and is `host_mock`-tested. The web app shows it and sends the operator's moves; it orders nothing itself.

## Decided

* **Hold a station that is working someone else,** with its tries frozen. Opening calls only (REPLYING); later stages are not held, since the evidence says little about mid-QSO repeats.
* **No hold for a station that has gone unheard:** the opening limit covers it.
* **An opening call gets at most 4 tries.**
* **A pin outranks only our other unanswered calls,** never a station calling us or a QSO in progress. Manual moves are pin, up and down, and Next.
* **One ladder,** easy to reorder, set on the Station sheet and stored on the main board.
* **POTA and SOTA are rungs,** used at home too, in a later phase. Park to park ranks the same whoever initiated.
* **"Needed" (DXCC, grid, park) waits** for the log database (row [I32](ideas.md#I32)).
* **A station that calls CQ over our calls** gets no special treatment beyond the opening limit.

## Evidence

From the maintainer's WSJT-X `ALL.TXT`, 2026-05-28 to 2026-08-06: 75,525 decodes and 3,303 of our transmissions, mostly 20 and 40 m FT8. The log is private and not committed; the scripts that read it are owed (**Open**).

Our transmissions to a station, by what it was doing in the four slots before (answered = decoded within two slots):

| The station was | Our TX | Answered |
|---|---|---|
| Calling CQ | 731 | 60 (8.2%) |
| Calling us | 688 | 445 (64.7%) |
| Not heard in the four slots | 229 | 10 (4.4%) |
| **Working someone else** | **169** | **1 (0.6%)** |
| Just sent RR73, RRR or 73 to someone else | 90 | 5 (5.6%) |

* **Repeated opening calls:** of 449 runs, call 1 was answered 9.1%, call 2 3.9%, call 3 4.7%, call 4 1.4%, calls 5 and later 0 of 55.
* **After its RR73, RRR or 73 to someone else,** a station is next heard a median 2 slots later, and more often answers a station already calling it (2,414) than calls CQ (1,822): the slot right after its closing message is when to call.
* **Callers to our CQ give up fast:** callers never answered called once (37), twice (23), three times (6) or more (8); 45% of those passed over in a pileup were worked within ten minutes.
* **A caller's SNR barely predicts completion:** 60–80% reached an R-report or RR73 in every 4 dB band from −24 to +11 dB.
* **A replay simulation** (the real band timeline drives everything but our station; an operator taps CQing stations into a queue of five, 30% of slots; 25.5 hours, 20 runs each): today's ordering 66.5 QSOs for 2,297 TX; 4 opening tries **70.2** for 2,045; holding busy stations 67.1 for 2,239, and +9% with an operator who taps twice as often; also holding stations unheard for 4 slots 64.1; also setting aside a station after two CQs over our calls 54.6.
* **Limits:** the answer rates are measured on the log the simulation replays, so it compares rules and does not forecast rates; one station's rates, probably at low power; mid-QSO repeats barely covered; FT8 only.

Comparable programs (read from source 2026-10-27): WSJT-X 3.0's Wait and Call, Wait and Reply, and CQ: First / Max Dist / Max dB; JTDX's priority ladder and "halt TX if the station I called answered another"; MSHV's multi-answer queue with sorting; FT8CN's watch list; the QMX panadapter's ranked auto-answer, pileup list and grey list.

## Design

### The order

The active zone sorts by these keys, in order; the first is today's rule.

1. **Stage:** IDLE first, then further along first, the CQ last.
2. **Held entries last within their stage,** so `queue[0]` is an entry that can go.
3. **A one-shot Next** ahead of everything but the transmission on the air, for one transmission.
4. **Pins, within REPLYING only.** A station calling us is answered 65% of the time and leaves within a call or two; an opening call 9% at best.
5. **Manual moves** within the stage.
6. **The ladder:** the first rung that tells two entries apart decides.

`sort_and_clean()` still runs where it runs today, plus after an operator move. Every key is a field on `QsoContext`, so one comparison function stays the whole rule and `host_mock` can drive every case.

### Opening tries ([jqpm](#jqpm))

An opening call gets `min(tries, AUTOSEQ_OPENING_TRIES)`, the constant 4; every later stage keeps the Station key `tries`. An entry's own count overrides it, as today. No new setting: one Tries on the Station sheet, and the limit is a constant named once and pinned by a host test.

### The busy hold

* **Busy:** within the last 4 slots, the station was decoded sending a message that continues a QSO with another call (a grid, a report, an R-report, a Field Day exchange, to a call that is not ours and not CQ).
* **Free:** its newest such decode is a CQ (any modifier), an RR73, RRR or 73 to someone else, or a message to us. A caller to our CQ is free by definition.
* **A held entry transmits nothing and spends no tries;** its tries resume where they were. **Release** is the first decode that makes the station free: one from an RR73, RRR or 73 lands in the slot before ours, so the entry goes out in the next slot.
* **A hold ends after 5 minutes:** the entry ends as if its last try were used ([autoseq.md](../architecture/autoseq.md), **The slot cycle**).
* **The data:** a per-call record of the station's last activity (slot, kind of message, to whom), filled from every decode as the heard-grid table is (`components/decode_pipeline/heard_grid.h`: 128 calls, 4 hours). One table or a sibling is an implementation choice. Today the engine sees only decodes addressed to us (`decode_pipeline_apply()`), so this is new input to it.
* **The operator sees** the row say it is held and why ("Working W1XYZ · waiting"), from one new `QUEUE_ENTRY` flag. The phone's own cue stays ([webapp.md](../architecture/webapp.md), **Decode rows and tap to reply**: CQ again, Working, Done with, Called by).
* **Matches WSJT-X's Wait and Call** in effect, without its three-call limit: the entry's tries bound the calls after release.
* **Mixed parity, test first:** two opening calls on opposite parities may alternate, keying every slot and missing each answer. A `host_mock` scenario checks it; if they alternate, the head's parity holds the period. `autoseq_rotate_same_parity()` has no caller today.

### Moving entries by hand

* **Pin** (a toggle): ahead of the other REPLYING entries, several in the order pinned; ends when the entry leaves REPLYING.
* **Up and down** swap with a neighbour in the same stage, kept as a manual rank the ladder does not override, cleared on a stage change; across a stage boundary refused, and not offered.
* **Next** puts the entry first for its next transmission, behind one already on the air; the one action that may outrank a QSO in progress, for one transmission.
* **Port A:** one `QUEUE_ORDER` message, `entry_id` plus the action, addressed as `QUEUE_CANCEL` and `QUEUE_TRIES` are; refusals carry a reason.
* **Web app:** the actions go in the sheet a row's tries count opens (`#opTriesEdit`), which becomes the entry's sheet; Operate shows about two queue rows, rebuilt every poll. A pinned row shows a pin mark.

### The ladder

* **A Station key, `queue_order`:** an ordered list of rung names, for example `far,tries,first`, on the main board like `tries`, applied at the next sort.
* **Rungs:** **Farthest** (Station grid to the station's grid, from the QSO or the heard-grid table; no grid last), **Strongest** (newest decode's SNR), **Fewest tries** (today's rule 4), **First heard** (creation order, always the final tie-break). Later, **POTA activator** and **SOTA activator**, from a heard-activation record per call (`CQ POTA`/`CQ SOTA`, kept for hours), and park to park while we activate (epic [I18](activator-mode.md)).
* **Default:** Farthest, Fewest tries, First heard. Not Strongest: SNR barely predicts completion.
* **Callers to our CQ** that answered in the same slot are at one stage with no tries sent, so the ladder alone picks who gets the first report: WSJT-X's CQ: First, Max Dist and Max dB as rungs.
* **Web app:** a Queue order list on the Station sheet with drag handles, one row per rung.

## Children

In order.

* <a id="jqpm"></a>**jqpm: opening calls get at most 4 tries** (**Opening tries**). **Done-when:** a host test shows REPLYING ends after 4 with `tries` at 6, after 2 with `tries` at 2, and after the entry's own count when one is set. **Lands in:** autoseq.md (**Tries**), and the evidence that sets 4 beside it; webapp.md if the row's count shows the cap.

### Later

* **Q1, the busy hold** (**The busy hold**). Done-when: `host_mock` scenarios hold an entry while its station works another call, release it on RR73 and transmit in the next slot, end a hold at 5 minutes, never charge a try while held, and settle mixed parity; a held entry is seen waiting on Operate and goes out after its station's RR73 (field).
* **Q4, the ladder and callers to our CQ** (**The ladder**). Done-when: host tests order two simultaneous callers by each rung; the Station sheet's order survives a reload and applies to the next sort.
* **Q3, manual order** (**Moving entries by hand**). Done-when: host tests for pin, up, down and Next, refusals across stages included; on the phone each action moves the row and changes what transmits next (field).
* **Q5, POTA and SOTA rungs** and park to park. Done-when: host tests mark a call from `CQ POTA` and order by the rungs, and P2P ranks the same whoever called. Whether the ladder follows the CQ type, as Field Day does, is settled then.
* **Live spots** as a second source for the activation rungs, once there is a path from the sidekick to the main board.
* **"Needed" rungs** after row [I32](ideas.md#I32).

## Open

* **The analysis and simulation scripts** behind **Evidence** are not in the repo; they read any WSJT-X `ALL.TXT` and belong in `tools/` beside a README, the log kept private. Before Q1 relies on them again.

## Why not

* **Holding a station not heard for some slots:** 10–30% fewer transmissions for about 5% fewer QSOs; 4 opening tries already bound calls to a station that has gone.
* **Setting a station aside after two CQs over our calls** (the panadapter's grey list, JTDX's counter): cut QSOs from 66.2 to 58.7 in the simulation. It exists for programs that choose stations on their own; here every opening call is a tap.
* **A pin above every entry:** trades a station calling us for one that has not answered; Next covers "this one now".
* **Strongest first by default,** and **a separate opening-tries setting** (one Tries setting, 2026-10-14; a constant does the job).
* **The ladder on the queue:** Operate shows two rows; the ladder is a station preference.
* **Several QSOs at once** (Fox, MSHV multi-answer): needs multi-tone TX and another on-air protocol.
* **Moving our TX offset after a CQ over us:** an idea only; the log held its offset, so nothing here can test it, and it has to fit row [B88](../ROADMAP.md#B88).
