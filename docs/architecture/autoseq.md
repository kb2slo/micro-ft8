# Autoseq: the QSO queue and the slot cycle

Autoseq decides what the station transmits next. It covers replying to stations that call us, working the stations the operator picks, the beacon CQ, and logging each QSO. The engine is `components/autoseq/` (`autoseq.h` is the API), shared by both main boards. The slot cycle that drives it has one copy per board: `check_slot_boundary()`, `tx_start()` and `tx_tick()` in `main/main.cpp` for the ADV, and `poe_p4/main/tx_scheduler.cpp` for the P4 (**not yet merged: row j4xh**). Clock and slot timing: [time.md](time.md).

The engine is a port of the DX-FT8 transceiver's `autoseq_engine.c`, and that lineage is where the two ordering rules under **The slot cycle** come from.

## The queue

* **One array, two zones.** `s_queue[AUTOSEQ_MAX_QUEUE]` (30 entries) holds a `QsoContext` per contact. The **active zone** grows from index 0 and is what gets transmitted. The **inactive zone** grows from the back and holds contacts that ran out of retries but whose metadata is still worth keeping (**Inactive zone** below). The free space between the zones is shared: a new entry evicts the oldest inactive one when the zones meet. A parked entry also expires 10 minutes after it was parked.
* **Only `queue[0]` transmits.** `autoseq_on_tx_starting()` logs `queue[0]` and records its `entry_id` as the entry on the air, and `refresh_tx_msg_buffer()` renders `queue[0]`'s text into a single buffer, which `autoseq_fetch_pending_tx()` reads. Every public call that can change `queue[0]` refreshes that buffer, so an empty queue and an empty buffer always go together.
* **Sort order** (`compare_ctx()`, applied to the active zone by `sort_and_clean()`, which then pops leading IDLE entries):
  1. IDLE first, so it is popped.
  2. A free-text one-shot next. **Nothing enqueues one any more:** `autoseq_schedule_freetext()` has no caller since one-shot free-text TX was removed.
  3. Then by state, further along first: SIGNOFF > ROGERS > ROGER_REPORT > REPORT > REPLYING > CALLING. A QSO near completion goes before a new one, and the beacon CQ comes last.
  4. **Within one state, fewer retries first.** Contacts at the same stage take turns rather than one burning all its retries while the others wait.
* **Stable IDs.** Each entry gets an `entry_id` when it is created, never reused within a boot, and carried along as the array re-sorts. Port A's `QUEUE_ENTRY` and `QUEUE_CANCEL` address entries by it, because the browser's snapshot and the live order can disagree by the time a cancel arrives ([webapp.md](webapp.md), **Screens**).
* **Sort only on the call paths that need it.** `sort_and_clean()` runs after decodes, a touch and a reactivation, not after `autoseq_tick()`. A tick only removes the entry it advanced if that entry finished, so the round-robin in rule 4 takes effect at the next decode.

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

* **`generate_response()` is total over (state, received).** Every cell is explicit, and "keep" rewrites `next_tx` to the canonical value. A message that classifies as `TX_NONE` returns before the table and changes nothing.
* **Reactivation restores `next_tx` itself.** `move_to_inactive()` clears `next_tx`, and `reactivate()` sets it back to the state's canonical value (`canonical_next_tx()`) before the table runs. Relying on the table's "keep" was not enough, because a message the classifier cannot place (no third field, say) returns before the table. An active entry with `TX_NONE` renders no text, so at `queue[0]` it stopped every TX, the beacon's included (`host_mock/test_queue_head.cpp`).
* **A new contact enters through the same table.** For a new entry, `generate_response(override=true)` first sets the state *before* the one the received message answers: TX1 → CALLING, TX2 → REPLYING, TX3 → REPORT, TX4 → ROGER_REPORT, TX5 → ROGERS. Then it applies the table. A grid answering our CQ lands in REPORT, and a straight report lands in ROGER_REPORT. The new entry takes DX's audio offset, and the slot parity opposite the one DX sent in.
* **`snr_tx`** (the report we send) is latched from the first TX1 or TX2 we decode from DX and never changes after that. A touch sets it from the tapped decode.
* **Field Day.** A contact is FD if the operator tapped a `CQ FD`, or if our CQ type is FD and DX sent a valid exchange. TX2 and TX3 then carry our exchange (parsed from the CQ free-text setting) instead of a report, and the received exchange is kept for Cabrillo.

## Where entries come from

* **Decodes addressed to us.** After each RX decode, `decode_pipeline.cpp` builds the list of decodes whose first field is our call and calls `autoseq_on_decodes()`. It skips this while transmitting and while battery-halted. The ADV's IgnoreList drops decodes before autoseq sees them (the P4 has none yet, row j4xh). `on_decode()` then:
  1. finds an active entry for the sender and applies the table, re-anchoring its slot parity to the decode;
  2. else finds an inactive one, reactivates it with fresh retries, and applies the table. Parked entries past their 10 minutes are expired before any lookup, so a stale one is never found;
  3. else creates a new entry, **except** for `RR73`/`RRR`/`73` (a late signoff for a QSO we no longer hold), `R±NN` (**Inactive zone** below says why), and a station logged in the last 10 minutes. A grid, a report or an FD exchange starts a QSO.

  **The recently-worked check sits only at step 3**: `autoseq_set_recent_qso_callback()`, which each board sets to `decode_pipeline_recently_logged()`, the ADIF dedupe window (`kAdifLoggerDedupeWindowMs`). It must never filter before steps 1 and 2. A QSO is logged when we send RR73, so the station's 73, or an `R±NN` resent because they missed our RR73, arrives after the call is already "recent" and must still reach its entry. Filtering in the decode pipeline did exactly that, and the ROGERS entry resent RR73 on every retry (`host_mock/test_recent_qso.cpp`). The decode list still shows recent calls grey and sorts them last, and a touch is never filtered.
* **A touch** (the ADV's R-screen tap, Port A `QUEUE_REPLY`) calls `autoseq_on_touch()`:
  * If that call is `queue[0]` in a live state, the touch is ignored ("QSO in progress").
  * Otherwise every entry for that call, active or inactive, is removed and a fresh one is created.
  * A decode addressed to us enters through the table. Anything else (a CQ, or tail-ending someone's RR73 or 73) starts in REPLYING sending TX1, or REPORT sending TX2 with Skip TX1 on.
  * The new entry is moved to the front, or behind `queue[0]` while a TX is on the air, so the transmission in flight is not displaced.
* **The beacon.** When the queue is empty, `arm_from_autoseq_or_beacon()` adds a one-shot CQ on the beacon's parity (Even is slot parity 0, Odd is 1). Only one CQ is ever queued. Turning the beacon off or changing its parity cancels queued CQs, but leaves `queue[0]` alone while it is on the air (`autoseq_cancel_cq(skip_head)`). The CQ's text is rendered from the template at refresh time (`CQ`, `CQ SOTA`, `CQ POTA`, `CQ QRP`, `CQ FD`, or a free-text CQ), so a station or CQ-type change applies to a CQ already queued.
* **Dropping an entry** (the ADV's TX screen, Port A `QUEUE_CANCEL`) removes a CQ outright and moves a QSO to the inactive zone, so a late reply still finds its metadata. Dropping the entry that is on the air also stops the transmission (**The slot cycle**).
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

   Just before the trigger, if the armed TX's `entry_id` is no longer `autoseq_head_entry_id()`, the board discards it and re-arms from the queue. This covers any queue change that did not re-arm, such as a Port A `QUEUE_CANCEL` between the decode and the slot. The check waits while a decode runs, since the decode mutates the queue on the other core.

   A late start skips the tones it has missed. Tones are timed from the slot boundary, not from the start, so a late TX still ends on time.

**Why this ordering.** Two rules keep the trigger, the transmission and the tick on the same entry. The `entry_id` checks (the head check above, and the tick's lookup below) are the backstop for queue changes the rules do not cover:

* **A decode only arms; the boundary transmits.** If a decode started a TX directly, a second decode could re-sort the queue while the TX was being prepared, and the tick afterwards would advance a different entry. That is the failure the DX-FT8 design avoids.
* **The trigger waits for the previous slot's decode to be applied.** Otherwise a boundary reached before capture and decode finish (about 12.6 s into an FT8 slot) could fire a TX from the cycle before.

**`autoseq_tick()`** acts on the entry that went on the air (the `entry_id` recorded at TX start), wherever it now sits, not on `queue[0]`. A cancel can drop that entry mid-TX, and a touch can re-sort another entry ahead of it. Acting on `queue[0]` regardless charged the retry to a QSO that never transmitted. If the entry is gone (dropped or parked), the tick does nothing. Otherwise:

* CALLING → IDLE, popped: a CQ is one-shot.
* REPLYING through ROGERS: resend with `retry_counter` + 1 while under `retry_limit` (the Station.txt max-retry setting, default `AUTOSEQ_MAX_RETRY` = 5). Once exhausted, REPLYING is evicted and REPORT or later moves to the inactive zone.
* SIGNOFF → inactive zone, always, so a late RR73 within the next 10 minutes can still be answered with another 73.

**A TX cancel** (the ADV's backtick, Port A `TX_CANCEL`) stops the TX and leaves autoseq untouched. The cancelled TX does not count as sent, so no tick runs and no retry is used. The entry re-arms, but the trigger refuses the slot the cancel landed in, so one press stops it for the rest of that slot (row B21 is the bench check).

**A queue cancel** (the web app's row ×, Port A `QUEUE_CANCEL`) removes a CQ, or parks a QSO. If that entry is armed but not yet on the air, the head check before the trigger drops the armed TX and re-arms. If it is on the air (`autoseq_transmitting_entry_id()` names it and a TX is active), the cancel also stops the transmission, as a TX cancel does, so no retry is spent and the rest of that slot stays quiet. The ADV's TX-screen drop keys do the same (`drop_qso()`). A band change sends a TX cancel as well as a queue cancel for every QSO.

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

Logging also starts the 10-minute recently-worked window for that call, so the station cannot start a second QSO with us until it passes. Its closing messages still reach the QSO (**Where entries come from**, step 3). Once the window and the parked entry's own 10 minutes have both passed, a call from that station is a new QSO, with a fresh report, and is logged again.

## Inactive zone

**The problem it solves.** Our retry window (a first TX plus 5 retries, every other slot: about 3 minutes) can be shorter than DX's. If an entry that had already sent a report were simply popped when its retries ran out, a late TX3 from DX would create a fresh entry with `snr_tx = −99`. That entry would complete the QSO and log the wrong `rst_sent`: the **reincarnation** bug (`host_mock/test_report_reincarnation.json`, `test_rogers_reincarnation.json`).

**The rules.**

* **Exhausted retries park, not pop,** once reports have been exchanged (state REPORT or later, `has_exchanged()`). The entry keeps its call, grid, both reports, `logged` and FD data. REPLYING (only our grid sent) and CALLING are evicted, since a fresh entry built from DX's next report gets the metadata right.
* **A decode from a parked contact reactivates it** at the end of the active zone with fresh retries, then applies the table. A dropped QSO is parked the same way.
* **An `R±NN` from an unknown contact is refused**, because a fresh entry would skip the `snr_tx` latch. TX1, TX2 and FD exchanges from unknown contacts are accepted, since each carries what a fresh entry needs. A late signoff from an unknown contact is ignored.
* **A parked entry expires `AUTOSEQ_INACTIVE_EXPIRY_MS` (10 minutes) after it was parked** (`inactive_since_ms`). `expire_inactive()` runs at the start of every `autoseq_on_decodes()`, before any lookup. `logged` plays no part.
  * **Why.** A parked entry exists to catch DX retrying the same QSO, and that happens within minutes. After that, a decode from the same call is a new QSO. Reviving the old entry would bring back its state, its reports and its `logged` flag. A parked SIGNOFF would answer a fresh grid with 73 on every call and never log the new QSO. A parked REPORT would send a report measured long before.
  * **Why 10 minutes.** It equals the recently-worked window (`kAdifLoggerDedupeWindowMs`, which `decode_pipeline.cpp` asserts is equal). For as long as the station is refused a new QSO, its parked entry still catches its retries. Once the window ends, it gets a fresh start. The two start at different moments, one when the QSO is logged and one when the entry is parked. A SIGNOFF entry parks in the same slot its 73 logs. A ROGERS entry that ran out of retries logged at its first RR73 but parked after its last, so its parked entry outlives the recently-worked window by that retry run (about 2.5 minutes at the default 5). During that gap, a fresh call from the station gets RR73.
* **When the queue is full, the oldest parked entry is evicted first**, whatever its age.
* **A touch purges** every entry for that call, parked ones included, so the operator's pick always starts clean.

## Tests

`host_mock/host_main.cpp` runs the JSON scenarios in `host_mock/` (a normal QSO, the beacon lifecycle, Field Day, the tail-ended RR73 touch, reincarnation and the two queue deadlocks). Each drives decodes, touches, beacon changes and ticks through the engine and **prints** the exchange. A scenario fails only on a crash, never on a wrong exchange ([TEST_PLAN.md](../TEST_PLAN.md)). The asserting tests are `test_unique_callsign.cpp` (touch de-duplication and `entry_id`), `test_beacon_cancel.cpp` (CQ cancellation), `test_recent_qso.cpp` (the recently-worked check blocks only new QSOs, and parked entries expire), and `test_queue_head.cpp` (the tick advances the entry that transmitted, and a reactivated entry can transmit). The slot cycle, the head check before the trigger, the IgnoreList and arming are board code, and no host test covers them.
