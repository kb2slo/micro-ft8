# RFC 0009: Operate — one station surface, home or activation

* **Status:** Accepted 2026-09-27. Decisions: §7. Slices and their rows: §6.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:**
  - the Operate view's layout, and a CQ row in the queue;
  - band and frequency controls, and one Station sheet;
  - adding a park or summit, from a list or the map;
  - radio status in place of Connect, and the TX HUD;
  - decode sort and filter, and reply feedback;
  - how an activation ([RFC 0008](0008-activator-mode.md)) appears with no separate flow;
  - auto-reply, as a later phase.
* **Does not cover:** the activation's data, store, log and export (RFC 0008); the QSOs and Tools tabs.
* **Mockups** (both private to the operator):
  - **"Operate Dock Options"** (https://claude.ai/artifact/HazeSpixV3UbDfUJDKqzGD). Its **chosen** page is this design; its **Explored** page keeps the rejected dock variants.
  - **"Operate Redesign"** (https://claude.ai/artifact/GsMDrsrNLEtnUC7aVNxptF), the first pass. Its decode sort and filter, reply feedback and radio states are built, drawn there in the rejected A/B/C chrome.

---

## 1. Goal

Stated by the operator, 2026-09-26 and 2026-09-27:

* **An activation is not a special flow off to the side.** Whatever activator mode adds must integrate cleanly with operating at home.
* **Rethink Connect.** It is a carry-over from the original Mini-FT8. The radio can now be autodetected, so connecting can probably be automatic.
* **Band and frequency are a primary action on Operate**, and a frequency can be typed in by number.
* **CQ type, frequency and retries are changeable from Operate,** and the CQ settings (type, grid, timeout) are one tap from the CQ in the queue.
* **Decodes can be ordered and filtered**, for example by distance, or by highest or lowest SNR.
* **Reply gives feedback at once.** Today the web app does not show a reply was taken until the main board's event comes back.
* **Parks and summits can be chosen on the map**, with the area around them saved for offline.
* **Medium term: auto-replies** to POTA, SOTA or DX stations, by a selectable ruleset.
* **Activation progress shows only when the station has references**, not at home (2026-09-26).

## 2. What exists

What the unbuilt slices start from, checked against the code on 2026-10-14. Operate as built: [webapp.md](../architecture/webapp.md), **Screens**.

* **Reply.** The button posts `/api/queue/reply` and does not change. The queue row appears only when a `QUEUE_ENTRY` event is polled, which the P4 emits once a second (`porta_p4_companion_status_tick()`), plus the browser's poll interval. A "Queueing…" message line is the only feedback.
* **The map** (`map.html`):
  - It loads POTA parks per program and SOTA summits per region while online, into memory only.
  - Parks and summits draw identically, at most 40 spread evenly across the view.
  - A tap opens a popup with name, reference, distance and a link to the official page.
  - Deep links: `?grid=`, `?lat=&lon=` and `?layer=psk`.

### 2.1 Settings writes, fixed with this draft

Found while tracing the operator's retries report (5 changed to 8, a new reply still showing `/5`); the fix shipped in `5b69795`.

* **The sidekick's `PUT /api/config` stopped at the first key the host refused.**
  - The web app saves every field at once, `band_sel` comes before `autoseq_max_retry`, and the P4 refuses `band_sel` whenever the VFO cannot move ("band saved, VFO not moved (TX active)").
  - So a save made mid-QSO dropped every later key, and the form still showed the typed value.
  - The sidekick now applies every line and reports the first refusal; the web app reloads the form after a failed save.
  - This was the cause of the report: on the P4 bench (2026-09-28) a save made during TX applied the new limit, and the next QSO used it.

## 3. Design

### 3.1 The principle: the station is the context, references are optional

There is no mode. An activation is the station having references (RFC 0008 §4.1). The references appear in the Station sheet and as a progress line under the slot bar; the rest of Operate reads them where they matter:

* P2P and S2S badges on decode rows.
* A "worked on this reference today" dupe cue.
* A partner reference on queue rows.

Clearing the references is the whole of ending an activation.

### 3.2 The layout

Built: the dock layout (O1, 2026-09-28), then layout v2 (O8, §3.14), whose regions are [webapp.md](../architecture/webapp.md), **Operate's pinned regions**.

### 3.14 Layout v2: status on top, controls at the bottom

Shipped as O8, 2026-10-25: [webapp.md](../architecture/webapp.md), **Operate's pinned regions**, **The radio bar**, **Decode rows and tap to reply** and **Band and frequency**; the phone check is in TEST_PLAN Pending. Its two open items, decided at the build (§7):

* **CAT lost mid-transmission:** Transmitting wins, so Stop stays where it was; the sub-line adds "CAT lost", and the bar shows CAT lost once the transmission ends.
* **Where Hz shows:** on the station's card, beside each decode heard this session.

### 3.5 The Station sheet: what's still to add

Built as O1c ([webapp.md](../architecture/webapp.md), **The Station sheet**). Still to add:

* **Activation:** the references, each with × and an **unverified** badge where it applies; **Add a park or summit** (§3.9). CQ follows the references until the type is picked by hand: a park gives POTA, and any summit gives SOTA (RFC 0008 §4.4).
* **The grid check against the references and the phone's fix** (RFC 0008 §4.3), on top of the sheet's own format check.

### 3.7 Radio status replaces Connect

Shipped as O2, now on the radio bar: [webapp.md](../architecture/webapp.md), **The radio bar**. Choosing the radio model stays in Settings, and the P4 ignores it today (row `cgc4`).

### 3.8 Decode sort and filter

Shipped as O4: [webapp.md](../architecture/webapp.md), **Decode sort and filter**. **New** waits on row `I32`.

### 3.9 Adding a park or summit

The UI half of RFC 0008 §4.2–4.3. Dock Options canvas: `Add a park or summit`, `Add, offline: typing a reference`, and the three `Map ·` boards, all interactive.

* **The add screen**, from the Station sheet:
  - Search by reference or name across the whole store (RFC 0008 §4.2), with an All / POTA / SOTA filter.
  - The nearby list is sorted by distance from the phone's fix, showing distance, bearing and summit points, with an **Add / Added** toggle on each row. An n-fer is a few taps.
  - A **Saved for offline** card shows what is saved and how old it is, with **Choose on the map ›**.
  - **A well-formed reference not in the store** adds as **unverified**. The program comes from the format: a slash means SOTA. **Malformed input** gets the format hint instead (RFC 0008 §4.3).
  - A standing caveat: distances are to each reference's listed point, not its boundary.
  - **Done · N added** returns to the sheet.
* **The map in pick mode** (`map.html?pick=refs&lat=…&lon=…`, beside today's `?grid=` and `?lat=&lon=`):
  - **Zoomed out:** your pin with its accuracy ring. **Save this area for offline** writes what the map already loaded for the view into the store (RFC 0008 §4.2) and draws the area's outline.
  - **Zoomed in:** labels on. A park or summit's popup gains **Add to activation**, and an added one turns blue. **Done · N** returns.
  - **Offline:** only saved areas draw, and saving is disabled with the reason.
* **Changes to the existing map:**
  - Parks draw as circles and summits as diamonds (today both are hollow circles); added ones fill blue.
  - The 40-marker cap must never hide a reference near your pin.
  - Saved areas get an outline layer, and offline the store is the POTA/SOTA source.
  - The popup always offers **Add to activation**, however the map was opened (decided 2026-09-27).
* **Honest limit:** the basemap is Natural Earth 110 m, so at park zoom the background is nearly blank: your pin, grid squares and markers. That is enough to pick by distance, not to judge "am I inside?". Park boundaries are a later phase (RFC 0008 §6).

### 3.10 Activation on Operate

Only when references exist (RFC 0008 §4.6):

* **A progress line under the slot bar:** each reference against its threshold (POTA 10, SOTA 4); the countdown to 0000Z is on the radio bar. Built with O8 ([webapp.md](../architecture/webapp.md), **Activation progress**).
* The references themselves live in the Station sheet (§3.5).
* P2P and S2S badges on decodes that carry a spotted reference, and a partner reference on queue rows.
* A dupe cue on decodes worked on this reference today.

Nothing moves between home and activation: the top grows by one line, and the CQ row's text changes.

### 3.12 The TX HUD

Shipped as O2, now the radio bar's Transmitting state: [webapp.md](../architecture/webapp.md), **The radio bar**. Stop CQ / Stop TX shipped with O1a ([webapp.md](../architecture/webapp.md), **The CQ row**).

### 3.13 Reply feedback

Shipped as O3, on the decode row since O8: [webapp.md](../architecture/webapp.md), **Decode rows and tap to reply**. The refusal of a reply to a decode heard on another frequency is in **Band and frequency**.

## 4. Rejected alternatives

* **An Activation screen, tab or mode.** The operator asked for integration (§1). A mode would add an error state for no gain, the same reasoning [webapp.md](../architecture/webapp.md) gives for no mode control.
* **A: a station card under the status strip.** At an activation the card grows by about 290 px and pushes decodes off screen when the activation is busiest.
* **C: a sideways tile rail.** It hides state off screen.
* **A one-row dock** with a band chip, and **a pull-up dock.** Both put band two taps away, and the pull-up dock, open, leaves about two decode rows visible.
* **A one-row dock** and **a pull-up dock** were rejected on 2026-09-27; the operator took the pull-up shape back on 2026-10-25 for the radio drawer, accepting band as two taps (§3.14).
* **Swiping the band row** past six bands. It hides bands behind a swipe, which undoes band as a one-tap action.
* **CQ controls in the dock** (the dock layout's first version) and **a separate CQ settings sheet.** CQ's status belongs with the queue it waits on, and two sheets split one set of settings.
* **Keeping Connect as a button.** Autodetect and the P4's sync on CAT open already do its job; a manual retune survives in the radio drawer.
* **A Save button for Operate controls.** Mid-QSO a control should take effect when touched, and batching is what caused §2.1.
* **A dialog confirming a reply was taken.** It needs a tap to dismiss on a 15 s clock and covers the decodes, while the row tapped is where the operator is already looking (§3.13).
* **A high-SWR warning in the TX HUD** (dropped 2026-09-27). The QMX protects itself, and a browser warning adds nothing to that. The HUD still shows the SWR reading.

## 5. Later phases

* **Auto-reply rules** (Redesign canvas: `Auto-reply rules`):
  - Rules such as CQ POTA or CQ SOTA from references not worked today, new DXCC entities, or a watch list.
  - Limits: minimum SNR, auto-replies queued at once, and a pause after a period with no touch.
  - An auto-reply is an ordinary queue entry, so × cancels it.
  - **Before building:** record a stance on control-operator presence. The station keys the transmitter without a tap for each QSO, and the licensee answers for it ([system.md](../architecture/system.md), **Who may key the transmitter**). Row `r2bu`.
* **Layouts for larger screens:** row `v86p`.

## 6. Slices

O1 ships as three pieces, each bench-checked on its own (decided 2026-09-27); they are the Now rows. O5 and O6 became one slice, the CQ's tries (§7). The combined order with RFC 0008 is RFC 0008 §7.

* **O1a: the CQ row**, shipped 2026-09-28 ([webapp.md](../architecture/webapp.md), **The CQ row**).
* **O1b: the dock**, shipped 2026-09-28; its band and frequency rules moved to the radio drawer with O8 ([webapp.md](../architecture/webapp.md), **Band and frequency**).
* **O1c: the Station sheet**, shipped 2026-09-28 ([webapp.md](../architecture/webapp.md), **The Station sheet**).
* **O2: radio status replaces Connect, the read-back, and the TX HUD**, shipped 2026-10-24, on the radio bar since O8 ([webapp.md](../architecture/webapp.md), **The radio bar**); its field check is in TEST_PLAN Pending.
* **O3: reply feedback**, shipped 2026-10-24, on the decode row since O8 ([webapp.md](../architecture/webapp.md), **Decode rows and tap to reply**); its check is O8's TEST_PLAN row.
* **O4: decode sort and filter**, shipped 2026-10-24 ([webapp.md](../architecture/webapp.md), **Decode sort and filter**); the phone check is in TEST_PLAN Pending.
* **O5 and O6: the CQ's tries and tries on every queue row**, shipped 2026-10-14 ([autoseq.md](../architecture/autoseq.md), **Tries**; [webapp.md](../architecture/webapp.md), **The CQ row** and **Tries on a queue row**).
* **The Activation section and the add screen ship with RFC 0008's slices:**
  - A1 brings typed references.
  - A3 brings the store, the nearby list and search, and the map's pick mode.
* **O8: layout v2**, shipped 2026-10-25 (§3.14), with RFC 0008's A2 progress line; the phone check is in TEST_PLAN Pending.
* **O7, later:** auto-reply (§5), row `r2bu`.

## 7. Decisions

**2026-09-27, before acceptance:**

* The dock layout (§3.2).
* The radio is read back on panel open and after each sync (§3.7).
* The map popup always offers **Add to activation** (§3.9).

**2026-09-27, at acceptance:**

* Decodes sort within each slot, and slots stay newest first (§3.8).
* Reply feedback is on the Reply button and in the queue, matched by call, with no dialog and no protocol change (§3.13).
* Per-QSO tries run 1–20, and raising the count on a parked entry does not revive it ([autoseq.md](../architecture/autoseq.md), **Tries**).
* No high-SWR warning (§4).
* O1 ships as three pieces, O1a to O1c (§6).

**2026-10-14, groom:**

* O2 includes the attach event before CDC opens that the Found state needs (§6).
* O4's CQ filter is the community's CQ-only ask (§3.8).
* **Decode sort and filter** (§3.8): the default sort is named Default, since within a slot every decode has the same time and "Newest" sorted nothing; New waits on row `I32`; POTA / SOTA means `CQ POTA` and `CQ SOTA` calls; a decode with no grid fails the distance filter.
* **CQ is a queue entry with a try budget.** Call CQ queues a CQ with N tries; each CQ sent uses one, an answer gives none back, and at zero it ends. Every queue entry's count can be changed in place. This replaces the beacon mode, O5's per-QSO retries and O6's CQ timeout. Built ([autoseq.md](../architecture/autoseq.md), **Tries**).
* **Call CQ picks its parity each time:** the active QSO's parity if there is one, otherwise the next slot it can still key. The CQ row shows it, and a tap flips it for that CQ, for a strong station nearby; SOTA spotting also wants the parity (RFC 0008 §3). Built (same).

**2026-10-24, decode controls:**

* The Decodes controls are one non-wrapping row of same-shape pills: Sort (opens its four choices), Replyable, POTA/SOTA, and "N of M" right-aligned; an active pill is filled.
* **Replyable replaces the CQ filter** and keeps the lines that carry a Reply button, by `isReplyableDecode()` and not by the clock.
* **The distance filter is dropped:** Nearest and Farthest already order by distance, and it hid every decode with no known grid.

**2026-10-14, building the CQ's tries:**

* **Every row counts tries, transmissions,** not retries. The Station key `tries` (default 6, the sends that 5 retries gave) replaces `autoseq_max_retry`.
* **One setting** bounds each QSO message and is what Call CQ starts with; there is no separate CQ count.

**2026-10-25, layout v2:**

* Status on top, controls at the bottom (§3.14): the slot bar alone is pinned above the decodes; queue, CQ row and a radio bar are pinned below.
* The radio bar carries radio state, transmit power and SWR, and Stop; band and frequency move into its drawer, so band is two taps.
* Decode rows are two lines, with no Reply button and no Hz; a tap on a replyable row replies, with an Undo toast.
* O8 ships as one slice and one commit; a bug found later is fixed forward.

**2026-10-25, building layout v2 (O8):**

* **CAT lost mid-transmission keeps Transmitting** and its Stop; the sub-line says CAT is lost, and the CAT lost state shows once the transmission ends. Stop stays where the thumb already is.
* **Hz shows on the station's card,** beside each decode heard this session: it is a detail of one station.
* **The app header and status line stay** above the slot bar; the canvas left them out, so the built decode list is about 66 pt shorter than drawn ([webapp.md](../architecture/webapp.md), **Operate's pinned regions**, measured).
* **The activation line ships with its counts** (RFC 0008 A2), since §3.14's top region is its home.
* **No Standard button** in the frequency row: a tap on the selected band already returns to its standard.
* **The slot header keeps the slot's span**, without the date ("14:02:30–45z · 6s ago"), per the 2026-09-28 decision that a start and an age counted from the end both read true.
* **Undo lasts 5 s,** and a reply to a call already queued gets none, since cancelling would end that QSO.
* **SWR tripped twice stays on the bar until the next transmission,** matching the main board, which stays stopped until the operator calls CQ, replies or tunes.
