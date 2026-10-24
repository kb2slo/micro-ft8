# Autoseq: the QSO queue and the slot cycle

Autoseq decides what the station transmits next. It covers replying to stations that call us, working the stations the operator picks, the CQ, how many times each goes out, and logging each QSO. The engine is `components/autoseq/` (`autoseq.h` is the API), The slot cycle that drives it is `poe_p4/main/tx_scheduler.cpp`. Clock and slot timing: [time.md](time.md).

The engine is a port of the DX-FT8 transceiver's `autoseq_engine.c`, and that lineage is where the two ordering rules under **The slot cycle** come from.

## The queue

* **One array, two zones.** `s_queue[AUTOSEQ_MAX_QUEUE]` (30 entries) holds a `QsoContext` per contact. The **active zone** grows from index 0 and is what gets transmitted. The **inactive zone** grows from the back and holds contacts that ran out of tries but whose metadata is still worth keeping (**Inactive zone** below). The free space between the zones is shared: a new entry evicts the oldest inactive one when the zones meet. A parked entry also expires 10 minutes after it was parked.
* **Only `queue[0]` transmits.** `autoseq_on_tx_starting()` logs `queue[0]` and records its `entry_id` as the entry on the air, and `refresh_tx_msg_buffer()` renders `queue[0]`'s text into a single buffer, which `autoseq_fetch_pending_tx()` reads. Every public call that can change `queue[0]` refreshes that buffer, so an empty queue and an empty buffer always go together.
* **Sort order** (`compare_ctx()`, applied to the active zone by `sort_and_clean()`, which then pops leading IDLE entries):
  1. IDLE first, so it is popped.
  2. A free-text one-shot next. **Nothing enqueues one any more:** `autoseq_schedule_freetext()` has no caller since one-shot free-text TX was removed.
  3. Then by state, further along first: SIGNOFF > ROGERS > ROGER_REPORT > REPORT > REPLYING > CALLING. A QSO near completion goes before a new one, and the CQ comes last.
  4. **Within one state, fewer tries sent first.** Contacts at the same stage take turns rather than one burning all its tries while the others wait.
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

**Classifying what DX sent** (`parse_rcvd_msg()`, on the third field): `RR73` or `RRR` is TX4, and `73` is TX5. An ARRL Field Day exchange (`1B SCV`, or `R 1B SCV`) counts as TX2, or TX3 with the `R`. A four-character grid is TX1, and fills in `dxgrid` if it is still empty (`RR73` is excluded; it matches the shape). **A grid DX sent in this QSO or in the CQ we tapped is the QSO's grid.** A QSO with neither logs the grid DX last sent in any message we decoded (`autoseq_set_heard_grid_callback()`, the board's `decode_pipeline_heard_grid()`), so tail-ending an RR73 or a report logs one, and so does a QSO whose tapped CQ's entry ran out of tries before DX answered (**Inactive zone**). That table (`heard_grid.h`, in `components/decode_pipeline`) takes a grid only from a standard message (FT8 types 1 and 2; free text can spell anything) whose field after the sender is a Maidenhead square: a CQ, with or without a modifier such as `CQ DX` or `CQ POTA` (`decode_fields.h` puts the sender in `field2` either way), or a reply to anyone. It keeps one entry per call, 128 calls (a full table drops the call heard longest ago), each kept 4 hours from when it was last heard. Its times are uptime, so a reboot empties it and it never waits on the clock being set ([time.md](time.md)). The main loop notes every decode in it (`decode_pipeline_apply()`) and reads it when a QSO logs, so it is not locked ([main_board.md](main_board.md), **Tasks**). Nothing is filled when the call was not heard sending a grid: the log row then shows no grid, region or distance. **Differs from WSJT-X** by decision (operator, 2026-10-23): its DX grid is set only from a grid in the tapped message and cleared when the base call changes (`widgets/mainwindow.cpp` at `967c85a`, line 9414), and its one fallback is a user's `CALL3.TXT`, which this table stands in for. The web app's info-card grid cache lives in the browser and never reaches the log. `R±NN` is TX3 and `±NN` is TX2, either one setting `snr_rx`, within ±30. Anything else is `TX_NONE` and changes nothing.

**Transitions** (`generate_response()`). Rows are our state and columns are what DX sent. "→ X" means the state becomes X with fresh tries; "keep" means we resend the state's canonical message.

| We are in | DX sent TX1 | TX2 | TX3 | TX4 (RR73) | TX5 (73) |
|---|---|---|---|---|---|
| CALLING | → REPORT | → ROGER_REPORT | → ROGERS | keep | → IDLE |
| REPLYING | keep | → ROGER_REPORT | → ROGERS | → SIGNOFF | → IDLE |
| REPORT | keep | → ROGER_REPORT | → ROGERS | → SIGNOFF | → IDLE |
| ROGER_REPORT | keep | keep | keep | → SIGNOFF | → IDLE |
| ROGERS | keep | keep | → ROGERS (RR73 again) | → IDLE | → IDLE |
| SIGNOFF | keep | keep | keep | → SIGNOFF (73 again) | → IDLE |

* **`generate_response()` is total over (state, received).** Every cell is explicit, and "keep" rewrites `next_tx` to the canonical value. A message that classifies as `TX_NONE` returns before the table and changes nothing.
* **Reactivation restores `next_tx` itself.** `move_to_inactive()` clears `next_tx`, and `reactivate()` sets it back to the state's canonical value (`canonical_next_tx()`) before the table runs. Relying on the table's "keep" was not enough, because a message the classifier cannot place (no third field, say) returns before the table. An active entry with `TX_NONE` renders no text, so at `queue[0]` it stopped every TX, the CQ's included (`host_mock/test_queue_head.cpp`).
* **A new contact enters through the same table.** For a new entry, `generate_response(override=true)` first sets the state *before* the one the received message answers: TX1 → CALLING, TX2 → REPLYING, TX3 → REPORT, TX4 → ROGER_REPORT, TX5 → ROGERS. Then it applies the table. A grid answering our CQ lands in REPORT, and a straight report lands in ROGER_REPORT. The new entry takes DX's audio offset, and the slot parity opposite the one DX sent in.
* **`snr_tx`** (the report we send) is latched from the first TX1 or TX2 we decode from DX and never changes after that. A touch sets it from the tapped decode.
* **Field Day follows the CQ type**, as WSJT-X's "Special operating activity" does. With CQ type FD every contact is FD: a reply opens with our exchange (TX2), never a grid, TX2 and TX3 carry the exchange (the Station key `free_text`) instead of a report, and the received exchange is kept for Cabrillo. With any other type, a tapped `CQ FD` gets an ordinary reply. WSJT-X disables Tx1 in Field Day mode and takes its exchange from a setting, never from the CQ it answers (`widgets/mainwindow.cpp` at `967c85a`, lines 8241-8249 and 9637). **Differs from Wei's Micro-FT8,** where tapping a `CQ FD` made that contact FD whatever our CQ type, and a reply opened with our grid. An exchange `parse_fd_exchange()` rejects is still sent as typed (row u9nb).

## Where entries come from

* **Decodes addressed to us.** After each RX decode, the main loop (`decode_pipeline_apply()`, handed the slot by the decode task) builds the list of decodes whose first field is our call and calls `autoseq_on_decodes()`. It skips this while transmitting. The P4 has no IgnoreList yet (row j4xh). `on_decode()` then:
  1. finds an active entry for the sender and applies the table, re-anchoring its slot parity to the decode;
  2. else finds an inactive one, reactivates it with fresh tries, and applies the table. Parked entries past their 10 minutes are expired before any lookup, so a stale one is never found;
  3. else creates a new entry, **except** for `RR73`/`RRR`/`73` (a late signoff for a QSO we no longer hold), `R±NN` (**Inactive zone** below says why), and a station logged in the last 10 minutes. A grid, a report or an FD exchange starts a QSO.

  **The recently-worked check sits only at step 3**: `autoseq_set_recent_qso_callback()`, which the P4 sets to `decode_pipeline_recently_logged()`, the ADIF dedupe window (`kAdifLoggerDedupeWindowMs`). It must never filter before steps 1 and 2. A QSO is logged when we send RR73, so the station's 73, or an `R±NN` resent because they missed our RR73, arrives after the call is already "recent" and must still reach its entry. Filtering in the decode pipeline did exactly that, and the ROGERS entry resent RR73 on every retry (`host_mock/test_recent_qso.cpp`). The decode list still shows recent calls grey and sorts them last, and a touch is never filtered.
* **A touch** (Port A `QUEUE_REPLY`) calls `autoseq_on_touch()`:
  * If that call is `queue[0]` in a live state, the touch is ignored ("QSO in progress").
  * Otherwise every entry for that call, active or inactive, is removed and a fresh one is created.
  * A decode addressed to us enters through the table. Anything else (a CQ, or tail-ending someone's RR73 or 73) starts in REPLYING sending TX1, or REPORT sending TX2 with Skip TX1 on.
  * The new entry is moved to the front, or behind `queue[0]` while a TX is on the air, so the transmission in flight is not displaced.
* **Call CQ** (Port A `CQ_CALL`) queues one CQ entry with the default's tries (**Tries** below). Only one CQ is ever queued; a second Call CQ is refused and leaves the first as it is. The CQ's text is rendered from the template at refresh time (`CQ`, `CQ SOTA`, `CQ POTA`, `CQ QRP`, `CQ FD`, or a free-text CQ), so a station or CQ-type change applies to a CQ already queued.
* **Dropping an entry** (Port A `QUEUE_CANCEL`, a row's ×) removes a CQ outright and moves a QSO to the inactive zone, so a late reply still finds its metadata. Dropping the entry that is on the air also stops the transmission (**The slot cycle**).
* **Rotate** (`autoseq_rotate_same_parity()`) moves `queue[0]` behind the other entries of the same parity at the front of the queue.

## Tries

Every queue entry counts its tries (transmissions, `tries_sent`) against a limit (`try_limit`). Decided 2026-10-14 ([RFC 0009](../rfcs/0009-operate-station-surface.md) §7).

* **A QSO's count is per step.** Each state change starts it over (`set_state()`), so the limit bounds how many times each message goes out. At the limit the entry ends as the tick says (**The slot cycle**): parked once reports are exchanged, dropped otherwise.
* **A CQ's count is the whole CQ.** Each CQ sent uses a try, an answer gives none back, and at its limit the CQ ends. It sorts below every QSO, so an answer is worked first and the CQ then resumes with the count it had. So nothing the station sends on its own goes out without bound.
* **The default is the Station key `tries`** (1–20, default 6, `AUTOSEQ_TRIES`, `autoseq_set_tries()`). It bounds each QSO message and is what a Call CQ starts with: one setting, not a separate CQ count (operator, 2026-10-14). A change applies at once to every entry without a count of its own, the queued CQ included.
* **An entry's own count** (`autoseq_set_entry_tries()`, Port A `QUEUE_TRIES`, a row's count in the web app): 1–20 for a QSO, 1–99 for the CQ, and 0 returns it to the default. It is kept in `tries_custom`, carries across the entry's steps and a reactivation, and a change of the default leaves it alone. Only an active entry can be changed: a parked one is not addressable, so raising a count never revives one.
* **Lowering a count to the tries already sent ends the entry** as its last try would (`retire_used_up()`): at once when it is not on the air, or at the tick after the try on the air, which still counts. A cancelled TX is not on the air (`autoseq_on_tx_cancelled()`), so lowering the count after a Stop ends the entry at once.
* **Call CQ picks its parity** (`autoseq_cq_parity()`): an active QSO's, so the CQ keeps the period the station already uses; otherwise the slot under way, if the trigger can still key it (its first 4/15, and not a slot a cancel or a skip refused), or else the next. The P4 picks, since it holds the slot clock (`tx_scheduler_call_cq()`). The CQ can be moved to the other period (`autoseq_set_cq_parity()`, Port A `QUEUE_PARITY`), for a strong station nearby; a QSO's parity is set by the station it works, so that is refused.
* **Differs from WSJT-X,** which repeats a message, a CQ included, until the operator acts or its Tx watchdog stops it. Here every message has a count; the decision is RFC 0009 §7's.

## The slot cycle

Each board runs `check_slot_boundary()` on every pass of its main loop. It does three things, in order:

1. **Tick after TX.** When a TX has finished (`was_txing` and no TX active), it calls `autoseq_tick()` once. A TX ends 12.64 s into its slot (FT8), so the tick lands there, before the next boundary, not at it.
2. **Arm.** If nothing is armed and no TX is in flight, it arms `queue[0]` (`tx_scheduler_arm_from_autoseq()`). The main loop also arms after applying every RX decode, including empty ones, so a quiet band cannot stall the CQ. A change the operator makes to the queue (Call CQ, the CQ's parity, an entry's tries) re-arms at once unless a TX is on the air (`tx_scheduler_rearm()`).
3. **Trigger.** It starts the armed TX when all of these hold:
   * the slot parity matches;
   * the slot is under 4/15 elapsed (4 s for FT8, 2 s for FT4);
   * no TX is on the air and no decode is running;
   * **the previous slot's decode has been applied** (`decode_applied_slot_idx >= slot_idx − 1`, and no decoded slot still waiting for the main loop, `decode_pipeline_result_pending()`);
   * the slot is not the one a TX cancel landed in, and not one `tx_start()` skipped (below).

   Just before the trigger, `autoseq_head_changed_or_refresh()` checks the armed TX against `autoseq_head_entry_id()`. A different entry (any queue change that did not re-arm, such as a Port A `QUEUE_CANCEL` between the decode and the slot) discards the armed TX and re-arms from the queue. The *same* entry gets its snapshot's text re-copied from the live `s_tx_msg_buffer` in place, rather than trusting what was armed — `autoseq_set_cq_type()` re-renders a queued CQ's text without changing its `entry_id`, so a `cq_type` switch made after this slot armed (RFC 0008 §4.4, found 2026-09-28) still lands in it instead of keying the stale text.

   **A slot that cannot be keyed is skipped, not counted** (`tx_start()` returns `Skipped`). That is: `radio_control_ready()` is false, `radio_control_begin_tx()` fails, the CPFSK start fails with anything but `ESP_ERR_NOT_SUPPORTED`, or an SWR sweep has the radio. `tx_start()` keys the radio before it calls `autoseq_on_tx_starting()`, so a skipped slot:
   * records no entry as transmitting and logs no QSO, so an RR73 that never went out is not logged;
   * sets no `was_txing`, so no tick runs and no try is charged;
   * writes no `T` line and never marks TX active, so nothing reads "Transmitting";
   * leaves the TX armed, so the entry goes out in its next slot once the radio answers;
   * is not tried again in that slot (`s_tx_skip_slot_idx`), and writes one `TX slot <n> skipped, not counted: <reason>` line to the console and the sidekick's log.

   An encode failure is the exception: it keys nothing but still charges the try, so a message that can never encode cannot loop forever. CAT lost partway through a TX that started keyed counts as sent. The phone shows a skipped slot as Not keyed for the rest of that slot ([webapp.md](webapp.md), **The radio chip and the TX banner**).

   A late start skips the tones it has missed, counted from the first tone's due time (see **TX timing on a QMX**, [radio.md](radio.md)). Tones are timed from the slot boundary, not from the start, so a late TX still ends on time.

**Why this ordering.** Two rules keep the trigger, the transmission and the tick on the same entry. The `entry_id` checks (the head check above, and the tick's lookup below) are the backstop for queue changes the rules do not cover:

* **A decode only arms; the boundary transmits.** If a decode started a TX directly, a second decode could re-sort the queue while the TX was being prepared, and the tick afterwards would advance a different entry. That is the failure the DX-FT8 design avoids.
* **The trigger waits for the previous slot's decode to be applied.** Otherwise a boundary reached before capture and decode finish (about 12.6 s into an FT8 slot) could fire a TX from the cycle before.

**`autoseq_tick()`** acts on the entry that went on the air (the `entry_id` recorded at TX start), wherever it now sits, not on `queue[0]`. A cancel can drop that entry mid-TX, and a touch can re-sort another entry ahead of it. Acting on `queue[0]` regardless charged the try to a QSO that never transmitted. If the entry is gone (dropped or parked), the tick does nothing. Otherwise:

* CALLING: the CQ counts the try and ends at its limit (**Tries**).
* REPLYING through ROGER_REPORT: count the try and resend while `tries_sent` is under `try_limit` (**Tries**). Once they are used, REPLYING is evicted and REPORT or later moves to the inactive zone.
* ROGERS → inactive zone after one RR73, once the QSO is logged.
  * **Why.** The QSO is logged with that RR73, and DX's 73 is optional: many stations send nothing, or go straight back to CQ. Blind resends went to a station that already had it, so ROGERS used every retry, about 2.5 minutes of RR73 at the default, while holding a queue place a new caller waited behind. A DX who missed our RR73 resends R+report, which reactivates the parked entry and gets RR73 again in the same slot an active entry would. Only a DX who missed it, and whose repeats we all miss for 10 minutes, gets no second one.
  * **Unless the log write failed.** An unlogged entry resends under its tries, as REPORT does, because each RR73 retries the write (**Logging**).
* SIGNOFF → inactive zone, always, so a late RR73 within the next 10 minutes can still be answered with another 73.

**A TX cancel** (Port A `TX_CANCEL`) stops the TX and leaves autoseq untouched. The cancelled TX does not count as sent, so no tick runs and no try is used (`autoseq_on_tx_cancelled()` clears the entry on the air). The entry re-arms, but the trigger refuses the slot the cancel landed in, so one press stops it for the rest of that slot (its bench check is in [TEST_PLAN.md](../TEST_PLAN.md) Pending). Ending the radio happens synchronously in the cancel call itself (`tx_scheduler_cancel()`), not on the next `tx_tick()`. The radio's SWR protection stopping a transmission is handled the same way, from `tx_tick()`: [radio.md](radio.md), **SWR protection trips on a QMX**.

**A queue cancel** (the web app's row ×, Port A `QUEUE_CANCEL`) removes a CQ, or parks a QSO. If that entry is armed but not yet on the air, the head check before the trigger drops the armed TX and re-arms. If it is on the air (`autoseq_transmitting_entry_id()` names it and a TX is active), the cancel also stops the transmission, as a TX cancel does, so no try is spent and the rest of that slot stays quiet. The CQ row's × does this for the CQ. A band change sends a TX cancel as well as a queue cancel for every QSO. Cancelling a QSO that has already exchanged a report (any state past `REPLYING`) does not evict it — `autoseq_drop_index()` parks it in the inactive zone the same way using its tries does, so a decode from the same call within the 10-minute inactive window reactivates it (`on_decode()`, `reactivate()`) with fresh tries rather than starting cold. This is deliberate: cancel means "pause," not "give up on this station."

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

`logged` stops a retry from logging twice. A failed write stays unlogged and retries on the next emission, so a ROGERS entry keeps resending RR73 until it logs or runs out of tries. A Field Day contact also writes a Cabrillo line first, with its own flag. A QSO that DX closes with 73 while we are still at ROGER_REPORT goes IDLE unlogged, because we never sent RR73 or 73.

Logging also starts the 10-minute recently-worked window for that call, so the station cannot start a second QSO with us until it passes. Its closing messages still reach the QSO (**Where entries come from**, step 3). Once the window and the parked entry's own 10 minutes have both passed, a call from that station is a new QSO, with a fresh report, and is logged again.

## Inactive zone

**The problem it solves.** Our window for a message (6 tries at the default, every other slot: about 3 minutes; a single RR73 for ROGERS) can be shorter than DX's. If an entry that had already sent a report were simply popped when its tries ran out, a late TX3 from DX would create a fresh entry with `snr_tx = −99`. That entry would complete the QSO and log the wrong `rst_sent`: the **reincarnation** bug (`host_mock/test_report_reincarnation.json`, `test_rogers_reincarnation.json`).

**The rules.**

* **Used-up tries park, not pop,** once reports have been exchanged (state REPORT or later, `has_exchanged()`). ROGERS parks after its one RR73 (**The slot cycle**). The entry keeps its call, grid, both reports, `logged` and FD data. REPLYING (only our grid sent) and CALLING are evicted: a fresh entry built from DX's next report takes a report measured from that answer, and the grid of a CQ we tapped comes back from the heard-grid table when it logs (**Classifying what DX sent**).
* **A decode from a parked contact reactivates it** at the end of the active zone with fresh tries, then applies the table. A dropped QSO is parked the same way.
* **An `R±NN` from an unknown contact is refused**, because a fresh entry would skip the `snr_tx` latch. TX1, TX2 and FD exchanges from unknown contacts are accepted, since each carries what a fresh entry needs. A late signoff from an unknown contact is ignored.
* **A parked entry expires `AUTOSEQ_INACTIVE_EXPIRY_MS` (10 minutes) after it was parked** (`inactive_since_ms`). `expire_inactive()` runs at the start of every `autoseq_on_decodes()`, before any lookup. `logged` plays no part.
  * **Why.** A parked entry exists to catch DX retrying the same QSO, and that happens within minutes. After that, a decode from the same call is a new QSO. Reviving the old entry would bring back its state, its reports and its `logged` flag. A parked SIGNOFF would answer a fresh grid with 73 on every call and never log the new QSO. A parked REPORT would send a report measured long before.
  * **Why 10 minutes.** It equals the recently-worked window (`kAdifLoggerDedupeWindowMs`, which `decode_pipeline.cpp` asserts is equal). For as long as the station is refused a new QSO, its parked entry still catches its retries. Once the window ends, it gets a fresh start. The two start at different moments, one when the QSO is logged and one when the entry is parked. A SIGNOFF entry parks in the same slot its 73 logs, and a ROGERS entry in the same slot as its RR73, so the two windows end within seconds of each other. An entry that a late message reactivates parks again later, and its 10 minutes restart then. Until they end, a fresh call from that station is answered from the old entry (RR73 or 73), not as a new QSO.
* **When the queue is full, the oldest parked entry is evicted first**, whatever its age.
* **A touch purges** every entry for that call, parked ones included, so the operator's pick always starts clean.

## Tests

`host_mock/host_main.cpp` runs the JSON scenarios in `host_mock/` (a normal QSO, the CQ's tries and its life in the queue, Field Day, the tail-ended RR73 touch, reincarnation and the two queue deadlocks). Further scenarios pin the paths those do not drive: CQ SOTA, POTA, QRP and free-text; a QSO opened by a report, with the sent report latched; `RRR` closing a QSO that never exchanged an R+report; a signoff or R+report from an unknown call; two callers, the further-along one first; a grid-only retry that is dropped; and Skip TX1. Each drives decodes, touches, Call CQ and its drop, and ticks through the engine. Every period carries `expect.tx` (empty when nothing is sent) and, when a QSO is logged, `expect.log` and `expect.cabrillo`; a mismatch fails the process. The same run emits the Port A event rows `GET /api/events` serves (`porta_event_json_*` in `components/porta_proto`, also called from `sidekick/main/host_link.c`). `test_qso.events` golden-checks that list. `host_mock/sim_station` plays a scenario on a wall clock and serves those rows, plus config, the queue (Call CQ, an entry's tries and the CQ's parity), and log, so `tools/serve_webapp_local.py --sim` can open Operate without a radio; past the script it keeps running empty periods, so a CQ called from the page goes out and counts. The asserting tests are `test_unique_callsign.cpp` (touch de-duplication and `entry_id`), `test_cq_tries.cpp` (the CQ's tries, an entry's own tries, lowering a count, Call CQ's parity), `test_recent_qso.cpp` (the recently-worked check blocks only new QSOs, and parked entries expire), `test_rogers_once.cpp` (ROGERS sends RR73 once, a resent R+report gets another, and a failed log write keeps it resending), `test_queue_head.cpp` (the tick advances the entry that transmitted, and a reactivated entry can transmit), and `test_heard_grid.cpp` (the heard-grid table, and the log filling an empty grid from it), with `test_decode_fields.cpp` pinning the field split it reads. The slot cycle, the head check before the trigger, the hand-off from the decode task, the IgnoreList and arming are board code, and no host test covers them. USB, CAT, and audio timing stay field-only.
