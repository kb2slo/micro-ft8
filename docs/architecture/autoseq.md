# Autoseq: the QSO queue and the slot cycle

Autoseq decides what the station transmits next. It covers replying to stations that call us, working the stations the operator picks, the beacon CQ, and logging each QSO. The engine is `components/autoseq/` (`autoseq.h` is the API), shared by both main boards. The slot cycle that drives it has one copy per board: `check_slot_boundary()`, `tx_start()` and `tx_tick()` in `main/main.cpp` for the ADV, and `poe_p4/main/tx_scheduler.cpp` for the P4 (**not yet merged: row j4xh**). Clock and slot timing: [time.md](time.md).

The engine is a port of the DX-FT8 transceiver's `autoseq_engine.c`, and that lineage is where the two ordering rules under **The slot cycle** come from.

## The queue

* **One array, two zones.** `s_queue[AUTOSEQ_MAX_QUEUE]` (30 entries) holds a `QsoContext` per contact. The **active zone** grows from index 0 and is what gets transmitted. The **inactive zone** grows from the back and holds contacts that ran out of retries but whose metadata is still worth keeping (**Inactive zone** below). The free space between the zones is shared: a new entry evicts the oldest inactive one when the zones meet.
* **Only `queue[0]` transmits.** `autoseq_tick()` advances `queue[0]`, `autoseq_on_tx_starting()` logs `queue[0]`, and `refresh_tx_msg_buffer()` renders `queue[0]`'s text into a single buffer, which `autoseq_fetch_pending_tx()` reads. Every public call that can change `queue[0]` refreshes that buffer, so an empty queue and an empty buffer always go together.
* **Sort order** (`compare_ctx()`, applied to the active zone by `sort_and_clean()`, which then pops leading IDLE entries):
  1. IDLE first, so it is popped.
  2. A free-text one-shot next. **Nothing enqueues one any more:** `autoseq_schedule_freetext()` has no caller since one-shot free-text TX was removed.
  3. Then by state, further along first: SIGNOFF > ROGERS > ROGER_REPORT > REPORT > REPLYING > CALLING. A QSO near completion goes before a new one, and the beacon CQ comes last.
  4. **Within one state, fewer retries first.** Contacts at the same stage take turns rather than one burning all its retries while the others wait.
* **Stable IDs.** Each entry gets an `entry_id` when it is created, never reused within a boot, and carried along as the array re-sorts. Port A's `QUEUE_ENTRY` and `QUEUE_CANCEL` address entries by it, because the browser's snapshot and the live order can disagree by the time a cancel arrives ([webapp.md](webapp.md), **Screens**).
* **Sort only on the call paths that need it.** `sort_and_clean()` runs after decodes, a touch and a reactivation, not after `autoseq_tick()`. A tick only pops `queue[0]` if it went IDLE, so the round-robin in rule 4 takes effect at the next decode.

## States and messages

A contact's state is **what we last sent**. `next_tx` is what we send next.

| State | We sent | Canonical `next_tx` |
|---|---|---|
| CALLING | CQ (a one-shot) | `TX_NONE`: text comes from the CQ template |
| REPLYING | TX1 `DX ME GRID` | TX1 |
| REPORT | TX2 `DX ME ±NN` | TX2 |
| ROGER_REPORT | TX3 `DX ME R±NN` | TX3 |
| ROGERS | TX4 `DX ME RR73` | TX4 |
| SIGNOFF | TX5 `DX ME 73` | TX5 |
| IDLE | done | `TX_NONE`; popped |

`TX6` (CQ) exists in the enum but is never assigned to `next_tx`.

**Classifying what DX sent** (`parse_rcvd_msg()`, on the third field): `RR73` or `RRR` is TX4, and `73` is TX5. An ARRL Field Day exchange (`1B SCV`, or `R 1B SCV`) counts as TX2, or TX3 with the `R`. A four-character grid is TX1, and fills in `dxgrid` if it is still empty (`RR73` is excluded; it matches the shape). `R±NN` is TX3 and `±NN` is TX2, either one setting `snr_rx`, within ±30. Anything else is `TX_NONE` and changes nothing.

**Transitions** (`generate_response()`). Rows are our state and columns are what DX sent. "→ X" means the state becomes X with fresh retries; "keep" means we resend the state's canonical message.

| We are in | DX sent TX1 | TX2 | TX3 | TX4 (RR73) | TX5 (73) |
|---|---|---|---|---|---|
| CALLING | → REPORT | → ROGER_REPORT | → ROGERS | keep | → IDLE |
| REPLYING | keep | → ROGER_REPORT | → ROGERS | → SIGNOFF | → IDLE |
| REPORT | keep | → ROGER_REPORT | → ROGERS | → SIGNOFF | → IDLE |
| ROGER_REPORT | keep | keep | keep | → SIGNOFF | → IDLE |
| ROGERS | keep | keep | → ROGERS (fresh retries) | → IDLE | → IDLE |
| SIGNOFF | keep | keep | keep | → SIGNOFF (73 again) | → IDLE |

* **`generate_response()` is total over (state, received).** Every cell is explicit, and "keep" rewrites `next_tx` to the canonical value. That is what repairs an entry coming back from the inactive zone with `next_tx = TX_NONE`. **The repair has a hole:** a received message that classifies as `TX_NONE` returns before the state machine runs, so a reactivated entry keeps `TX_NONE`. If it reaches `queue[0]`, its text is empty, `fetch_pending_tx()` returns false, and nothing transmits (not even the beacon, whose CQ sorts behind it) until the entry is removed.
* **A new contact enters through the same table.** For a new entry, `generate_response(override=true)` first sets the state *before* the one the received message answers: TX1 → CALLING, TX2 → REPLYING, TX3 → REPORT, TX4 → ROGER_REPORT, TX5 → ROGERS. Then it applies the table. A grid answering our CQ lands in REPORT, and a straight report lands in ROGER_REPORT. The new entry takes DX's audio offset, and the slot parity opposite the one DX sent in.
* **`snr_tx`** (the report we send) is latched from the first TX1 or TX2 we decode from DX and never changes after that. A touch sets it from the tapped decode.
* **Field Day.** A contact is FD if the operator tapped a `CQ FD`, or if our CQ type is FD and DX sent a valid exchange. TX2 and TX3 then carry our exchange (parsed from the CQ free-text setting) instead of a report, and the received exchange is kept for Cabrillo.

## Where entries come from

* **Decodes addressed to us.** After each RX decode, `decode_pipeline.cpp` builds the list of decodes whose first field is our call and calls `autoseq_on_decodes()`. It skips this while transmitting and while battery-halted. **Two filters drop decodes before autoseq sees them:** the ADV's IgnoreList (the P4 has none yet, row j4xh), and any station logged in the last 10 minutes (the ADIF dedupe window, `kAdifLoggerDedupeWindowMs`). `on_decode()` then:
  1. finds an active entry for the sender and applies the table, re-anchoring its slot parity to the decode;
  2. else finds an inactive one, reactivates it with fresh retries, and applies the table;
  3. else creates a new entry, **except** for `RR73`/`RRR`/`73` (a late signoff for a QSO we no longer hold) and `R±NN` (**Inactive zone** below says why). A grid, a report or an FD exchange starts a QSO.
* **A touch** (the ADV's R-screen tap, Port A `QUEUE_REPLY`) calls `autoseq_on_touch()`:
  * If that call is `queue[0]` in a live state, the touch is ignored ("QSO in progress").
  * Otherwise every entry for that call, active or inactive, is removed and a fresh one is created.
  * A decode addressed to us enters through the table. Anything else (a CQ, or tail-ending someone's RR73 or 73) starts in REPLYING sending TX1, or REPORT sending TX2 with Skip TX1 on.
  * The new entry is moved to the front, or behind `queue[0]` while a TX is on the air, so the transmission in flight is not displaced.
* **The beacon.** When the queue is empty, `arm_from_autoseq_or_beacon()` adds a one-shot CQ on the beacon's parity (Even is slot parity 0, Odd is 1). Only one CQ is ever queued. Turning the beacon off or changing its parity cancels queued CQs, but leaves `queue[0]` alone while it is on the air (`autoseq_cancel_cq(skip_head)`). The CQ's text is rendered from the template at refresh time (`CQ`, `CQ SOTA`, `CQ POTA`, `CQ QRP`, `CQ FD`, or a free-text CQ), so a station or CQ-type change applies to a CQ already queued.
* **Dropping an entry** (the ADV's TX screen, Port A `QUEUE_CANCEL`) removes a CQ outright and moves a QSO to the inactive zone, so a late reply still finds its metadata.
* **Rotate** (the ADV's TX screen, `autoseq_rotate_same_parity()`) moves `queue[0]` behind the other entries of the same parity at the front of the queue.

## The slot cycle

Each board runs `check_slot_boundary()` on every pass of its main loop. It does three things, in order:

1. **Tick after TX.** When a TX has finished (`was_txing` and no TX active), it calls `autoseq_tick()` once. A TX ends 12.64 s into its slot (FT8), so the tick lands there, before the next boundary, not at it.
2. **Arm.** If nothing is armed and no TX is in flight, it arms `queue[0]`, or a beacon CQ if the queue is empty. The decode pipeline also arms after every RX decode, including empty ones, so a quiet band cannot stall the beacon.
3. **Trigger.** It starts the armed TX when all of these hold:
   * the slot parity matches;
   * the slot is under 4/15 elapsed (4 s for FT8, 2 s for FT4);
   * no TX is on the air and no decode is running;
   * **the previous slot's decode has been applied** (`decode_applied_slot_idx >= slot_idx − 1`);
   * the slot is not the one a TX cancel landed in.

   A late start skips the tones it has missed. Tones are timed from the slot boundary, not from the start, so a late TX still ends on time.

**Why this ordering.** Two rules keep the tick and the trigger acting on the same entry:

* **A decode only arms; the boundary transmits.** If a decode started a TX directly, a second decode could re-sort the queue while the TX was being prepared, and the tick afterwards would advance a different entry. That is the failure the DX-FT8 design avoids.
* **The trigger waits for the previous slot's decode to be applied.** Otherwise a boundary reached before capture and decode finish (about 12.6 s into an FT8 slot) could fire a TX from the cycle before.

**`autoseq_tick()`** acts on `queue[0]`:

* CALLING → IDLE, popped: a CQ is one-shot.
* REPLYING through ROGERS: resend with `retry_counter` + 1 while under `retry_limit` (the Station.txt max-retry setting, default `AUTOSEQ_MAX_RETRY` = 5). Once exhausted, REPLYING is evicted and REPORT or later moves to the inactive zone.
* SIGNOFF → inactive zone, always, so a late RR73 can still be answered with another 73.

**A TX cancel** (the ADV's backtick, Port A `TX_CANCEL`) stops the TX and leaves autoseq untouched. The cancelled TX does not count as sent, so no tick runs and no retry is used. The entry re-arms, but the trigger refuses the slot the cancel landed in, so one press stops it for the rest of that slot (row B21 is the bench check).

**A queue cancel does not disarm.** `QUEUE_CANCEL` removes the entry but leaves an already-armed TX armed. Only the next decode's arm replaces it. If the cancel lands between the decode and the TX trigger, the cancelled message goes out once, and the tick after it advances whatever is now `queue[0]`.

**TX offset** is resolved when the TX is armed (`resolve_tx_offset()`):

* CURSOR: the operator's cursor.
* RX: DX's own offset, except for a CQ.
* RANDOM (the default), and RX for a CQ: a fresh 500–2500 Hz roll every arm.

Row B88 holds the offset for the length of a QSO.

## Logging

**A QSO is logged when we emit RR73 or 73**, from `autoseq_on_tx_starting()` in `tx_start()`, never on a received message:

* We only log QSOs we actually attempted to close.
* Each TX slot yields at most one log line, so stamps are unique.
* Both reports are latched by ROGERS or SIGNOFF.

`logged` stops a retry from logging twice. A failed write stays unlogged and retries on the next emission. A Field Day contact also writes a Cabrillo line first, with its own flag. A QSO that DX closes with 73 while we are still at ROGER_REPORT goes IDLE unlogged, because we never sent RR73 or 73.

**Logging arms the 10-minute recent-QSO filter** (**Where entries come from**), and that filter runs before autoseq. So once we have sent RR73, derived from the code and unconfirmed on air:

* DX's 73 never reaches the ROGERS entry, which keeps resending RR73 until its retries run out (default 5) and then parks.
* A re-sent `R±NN` from DX, which did not copy our RR73, is dropped too. The RR73 retries cover that case by accident.
* The SIGNOFF late-RR73 path cannot fire within the window.

The host scenarios feed `autoseq_on_decodes()` directly, so they never see this filter.

## Inactive zone

**The problem it solves.** Our retry window (a first TX plus 5 retries, every other slot: about 3 minutes) can be shorter than DX's. If an entry that had already sent a report were simply popped when its retries ran out, a late TX3 from DX would create a fresh entry with `snr_tx = −99`. That entry would complete the QSO and log the wrong `rst_sent`: the **reincarnation** bug (`host_mock/test_report_reincarnation.json`, `test_rogers_reincarnation.json`).

**The rules.**

* **Exhausted retries park, not pop,** once reports have been exchanged (state REPORT or later, `has_exchanged()`). The entry keeps its call, grid, both reports, `logged` and FD data. REPLYING (only our grid sent) and CALLING are evicted, since a fresh entry built from DX's next report gets the metadata right.
* **A decode from a parked contact reactivates it** at the end of the active zone with fresh retries, then applies the table. A dropped QSO is parked the same way.
* **An `R±NN` from an unknown contact is refused**, because a fresh entry would skip the `snr_tx` latch. TX1, TX2 and FD exchanges from unknown contacts are accepted, since each carries what a fresh entry needs. A late signoff from an unknown contact is ignored.
* **Eviction is oldest-parked first** (`inactive_since_ms`), and only when the queue is full. There is no expiry by time, and `logged` plays no part.
* **A touch purges** every entry for that call, parked ones included, so the operator's pick always starts clean.

## Tests

`host_mock/host_main.cpp` runs the JSON scenarios in `host_mock/`. Each drives decodes, touches, beacon changes and ticks through the engine and checks the queue and the log: a normal QSO, the beacon lifecycle, Field Day, the tail-ended RR73 touch, reincarnation and the two queue deadlocks. `test_unique_callsign.cpp` pins touch de-duplication and `entry_id`, and `test_beacon_cancel.cpp` pins CQ cancellation. The slot cycle, the decode filters and arming are board code, and no host test covers them.
