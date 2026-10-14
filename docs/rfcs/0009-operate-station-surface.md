# RFC 0009: Operate — one station surface, home or activation

* **Status:** Accepted 2026-09-27. Decisions: §7. Slices and their rows: §6.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:**
  - the Operate view's layout, and a CQ row in the queue;
  - the dock's band and frequency controls, and one Station sheet;
  - adding a park or summit, from a list or the map;
  - radio status in place of Connect, and the TX HUD;
  - decode sort and filter, and reply feedback;
  - how an activation ([RFC 0008](0008-activator-mode.md)) appears with no separate flow;
  - auto-reply, as a later phase.
* **Does not cover:** the activation's data, store, log and export (RFC 0008); the QSOs and Tools tabs.
* **Mockups** (both private to the operator):
  - **"Operate Dock Options"** (https://claude.ai/artifact/HazeSpixV3UbDfUJDKqzGD). Its **chosen** page is this design; its **Explored** page keeps the rejected dock variants.
  - **"Operate Redesign"** (https://claude.ai/artifact/GsMDrsrNLEtnUC7aVNxptF), the first pass. It is still the reference for decode sort and filter, reply feedback, the radio chip and the TX HUD states, drawn in the rejected A/B/C chrome.

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

What the unbuilt slices start from, checked against the code on 2026-10-14. Operate as O1 left it (the status header, the queue and its CQ row, decodes, the dock): [webapp.md](../architecture/webapp.md), **Screens**.

* **Connect** (`porta_host_connect()`) only pushes the band to the VFO. USB host starts at boot, and the main loop already re-syncs the VFO whenever CAT opens (`poe_p4/main/main.cpp`), so Connect is a manual retune and nothing else.
* **Attach and detach are already pushed.** `USB_STATUS` events reach the browser (`applyEvent`, `e.d === 5`) but only drive a banner.
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

There is no mode. An activation is the station having references (RFC 0008 §4.1). The references appear in the Station sheet and as a progress line in the dock; the rest of Operate reads them where they matter:

* P2P and S2S badges on decode rows.
* A "worked on this reference today" dupe cue.
* A partner reference on queue rows.

Clearing the references is the whole of ending an activation.

### 3.2 The layout: a bottom dock (decided 2026-09-27)

Top to bottom:

1. **Status strip:** the slot bar and parity, plus the radio chip (§3.7).
2. **Queue:** QSO rows (call, tries, state, ×), each count changeable in place ([webapp.md](../architecture/webapp.md), **Tries on a queue row**). Then the **CQ row**, always last ([webapp.md](../architecture/webapp.md), **The CQ row**).
3. **Decodes:**
   - **Rows exactly as today's app:** a mono meta line with DT and Hz in fixed left columns and dB right-justified; the message, with callsigns as links and the → ME badge; then grid · location on the left and distance and bearing on the right.
   - **The Reply button is the new style:** a 36 px outlined button.
   - Sort and filter sit in the Decodes header (§3.8).
4. **Dock:** the band grid and one line with the frequency and a Station link ([webapp.md](../architecture/webapp.md), **The dock**). At an activation, a progress line is added (§3.10).
5. **Tab bar.**

**Why the dock layout.** Decodes are the scarce resource on a phone, and the dock keeps its height fixed. Band is one tap. Every control touched mid-QSO is in thumb reach or in the queue. The rejected variants are in §4.

### 3.5 The Station sheet: what's still to add

Built as O1c ([webapp.md](../architecture/webapp.md), **The Station sheet**). Still to add:

* **Activation:** the references, each with × and an **unverified** badge where it applies; **Add a park or summit** (§3.9). CQ follows the references until the type is picked by hand: a park gives POTA, and any summit gives SOTA (RFC 0008 §4.4).
* **The grid check against the references and the phone's fix** (RFC 0008 §4.3), on top of the sheet's own format check.

### 3.7 Radio status replaces Connect

The radio chip shows one of five states (Redesign canvas: `Radio status`, and the interactive `Radio chip: tap for details`):

| State | Chip | Driven by |
|---|---|---|
| No radio | grey | no `USB_STATUS` attach |
| Unknown USB | grey, "Unknown USB" | attach of a device that is not a known radio (`USB_KIND_NAMES` falls back to VID/PID today) |
| Found | amber, "syncing" | attach seen; CAT and audio not both up yet |
| Ready | green, radio and band | `cat_ready` and `audio_rx` |
| Lost | red, "CAT lost" | CAT down after being up |

* **The Connect button goes.** The P4 already syncs the VFO on every CAT open (§2).
* **Tapping the chip opens a details panel under it.** It never navigates away from Operate, and tapping the chip again closes it. What the panel holds depends on the state:
  - **Ready:**
    - Rows: USB device, CAT, audio in, what was last sent to the radio and when, what the radio reports, and the last TX's power and SWR.
    - **What is sent.** A sync writes `MD6;` (Digi: "FSK" in the Kenwood table the QMX follows), `FR0;` `FT0;` (VFO A for RX and TX) and `FA<freq>;` (`components/radio_control/radio_control_qmx.cpp:26`). Nothing reads the radio back today.
    - **Read-back (decided 2026-09-27).** `IF;` when the panel opens and after each sync, never polled during a slot. One reply carries the frequency, TX/RX state, mode and VFO/split (QMX CAT manual, firmware 1.04_004).
    - **When the radio is not where Micro-FT8 put it** (a knob turned, another program), the chip turns amber and the panel offers **Put it back** (Retune) or **Use it** (save the radio's frequency as that band's custom frequency).
    - **Building the read-back:** a `radio_control` operation that sends `IF;` and parses the reply, with a capability flag; a `host_mock` test of the parser against the manual's format; a Port A request and reply.
    - **Retune VFO:** sends that frequency, Digi and VFO A again. It is today's P4 Connect (`porta_host_connect()` → `sync_vfo()`); a confirmation line shows what was sent.
    - **Tune carrier:** today's Tune button (`/api/radio/tune`), a carrier at the TX offset (`porta_host_tune()`). While keyed, the panel shows power and SWR large enough to read while adjusting a tuner. It is also a TX HUD state (§3.12).
  - **Found:** a checklist of USB, CAT, audio and VFO, with nothing to press.
  - **No radio:** where to plug in, and that a charge-only cable shows nothing.
  - **Unknown USB:** the VID/PID, and which radios are supported.
  - **Lost:** when CAT stopped answering, and **Retry now**. TX is not keyed until CAT answers, and a slot that cannot be keyed is skipped, not counted ([autoseq.md](../architecture/autoseq.md), **The slot cycle**).
* **Not in the panel:** choosing the radio model. It stays in Settings, and the P4 ignores it today (row `cgc4`).

### 3.8 Decode sort and filter

Redesign canvas: `Decode sort and filter`, interactive. On Operate the controls sit in the Decodes header.

* **Sort:** Default (today's order: decodes to us, then replyable lines with CQs strongest first, then the rest, then stations recently worked), Strongest (SNR), Nearest (distance). A decode with no known grid sorts after those with one.
* **Sorting applies within each slot, and slots stay newest first** (decided 2026-09-27). The 15 s rhythm stays visible, and replies go to the newest slot anyway.
* **Show only:** CQ, POTA / SOTA (`CQ POTA`, `CQ SOTA`), under a distance (a decode with no known grid fails it). Filters combine. **New** waits on row `xv23`: the app knows only the log it has loaded.
* **Messages addressed to the operator always stay on top**, and are never filtered out.
* **The choice is remembered per browser** (`localStorage`: a per-viewer convenience).

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

* **A progress line in the dock:** each reference against its threshold (POTA 10, SOTA 4), and the countdown to 0000Z.
* The references themselves live in the Station sheet (§3.5).
* P2P and S2S badges on decodes that carry a spotted reference, and a partner reference on queue rows.
* A dupe cue on decodes worked on this reference today.

Nothing moves between home and activation: the dock grows by one line, and the CQ row's text changes.

### 3.12 The TX HUD

Dock Options canvas: the chosen page's transmitting board. Redesign canvas: `TX HUD states`, interactive.

* **Today** (`renderTxHud()`):
  - A red band under the header: "Transmitting", the message, then power and SWR (polled from the QMX), and **Cancel TX**.
  - Tune shows as `TUNE`.
  - Events arrive every 500 ms while active.
* **It is a red band added on top of the dock.** Nothing below it moves, so band, frequency and the CQ row stay reachable while a message is on the air. The status strip's slot bar turns red for our slot.
* **Added to what shows today:**
  - The parity and TX offset (`offset_hz` is already in the event but not shown).
  - A progress bar and the time left, from the slot clock the browser already runs.
  - For Tune, large power and SWR readings.
  - **Stop CQ / Stop TX**, shipped with O1a ([webapp.md](../architecture/webapp.md), **The CQ row**).
* **New state, Not keyed:** CAT was not ready when the slot started. The message is struck through and "not counted" is shown. The P4 already skips and does not count the slot ([autoseq.md](../architecture/autoseq.md), **The slot cycle**); the state needs a Port A event, which is row ybav's.

### 3.13 Reply feedback

Redesign canvas: `Reply feedback`, interactive. The feedback is on the Reply button and in the queue, not in a dialog (decided 2026-09-27).

* **Tap Reply:** the button locks at once and reads "Sending…". The sidekick holds the POST until the main board accepts or refuses (`post_queue_reply()`, `sidekick/main/host_link.c:795`), so the answer takes about a second.
* **Accepted:** the button reads "Queued ✓", and a dashed placeholder for that call appears in the queue. When a `QUEUE_ENTRY` for the same call arrives, the placeholder becomes the real entry. Matching on the call needs no protocol change. If the call is already queued, that row is highlighted instead.
* **Refused, or no entry within 10 s:** the placeholder goes, and the reason shows on the decode row with a Retry button.
* **Already built** (2026-09-28): a reply to a decode heard on another frequency is refused in the app with a dialog ([webapp.md](../architecture/webapp.md), **The dock**).

## 4. Rejected alternatives

* **An Activation screen, tab or mode.** The operator asked for integration (§1). A mode would add an error state for no gain, the same reasoning [webapp.md](../architecture/webapp.md) gives for no mode control.
* **A: a station card under the status strip.** At an activation the card grows by about 290 px and pushes decodes off screen when the activation is busiest.
* **C: a sideways tile rail.** It hides state off screen.
* **A one-row dock** with a band chip, and **a pull-up dock.** Both put band two taps away, and the pull-up dock, open, leaves about two decode rows visible.
* **Swiping the band row** past six bands. It hides bands behind a swipe, which undoes band as a one-tap action.
* **CQ controls in the dock** (the dock layout's first version) and **a separate CQ settings sheet.** CQ's status belongs with the queue it waits on, and two sheets split one set of settings.
* **Keeping Connect as a button.** Autodetect and the P4's sync on CAT open already do its job; a manual retune survives behind the radio chip.
* **A Save button for Operate controls.** Mid-QSO a control should take effect when touched, and batching is what caused §2.1.
* **A dialog confirming a reply was taken.** It needs a tap to dismiss on a 15 s clock and covers the decodes, while the Reply button is where the operator is already looking (§3.13).
* **A high-SWR warning in the TX HUD** (dropped 2026-09-27). The QMX protects itself, and a browser warning adds nothing to that. The HUD still shows the SWR reading.

## 5. Later phases

* **Auto-reply rules** (Redesign canvas: `Auto-reply rules`):
  - Rules such as CQ POTA or CQ SOTA from references not worked today, new DXCC entities, or a watch list.
  - Limits: minimum SNR, auto-replies queued at once, and a pause after a period with no touch.
  - An auto-reply is an ordinary queue entry, so × cancels it.
  - **Before building:** record a stance on control-operator presence. The station keys the transmitter without a tap for each QSO, and the licensee answers for it ([system.md](../architecture/system.md), **Who may key the transmitter**). Row `r2bu`.
* **Layouts for larger screens:** row `v86p`.

## 6. Slices

O1 ships as three pieces, each bench-checked on its own (decided 2026-09-27); they are the Now rows. The later slices have their own rows: O2 `ybav`, O3 `mxvk`, O4 `jtgu`. O5 and O6 became one slice, the CQ's tries (§7). The combined order with RFC 0008 is RFC 0008 §7.

* **O1a: the CQ row**, shipped 2026-09-28 ([webapp.md](../architecture/webapp.md), **The CQ row**).
* **O1b: the dock**, shipped 2026-09-28 ([webapp.md](../architecture/webapp.md), **The dock**).
* **O1c: the Station sheet**, shipped 2026-09-28 ([webapp.md](../architecture/webapp.md), **The Station sheet**).
* **O2: radio status replaces Connect, the read-back, and the TX HUD** (§3.7, §3.12).
  - Web app; the `IF;` read-back operation, its parser test and its Port A message.
  - **An attach event before CDC opens.** The P4 sends `USB_STATUS` attach only once CDC opens (`poe_p4/main/qmx_transport.cpp`, `try_open()`), about 11 s after plug-in (a QMX-side delay, measured 2026-09-20), so today Found never shows and the wait reads as a hang. Found starts when the host core first sees the device (`cdc_new_dev_cb()`), not when `try_open()` starts polling: it polls about once a second whenever nothing is attached.
  - **Field check:** unplug and replug the QMX. The chip walks No radio, Found, Ready, Found showing within a second or two of plug-in, and the VFO lands on the selected band with nothing pressed. Then Tune from the chip's panel shows power and SWR there and in the HUD, and Stop in the HUD ends it.
* **O3: reply feedback** (§3.13). Web app only.
* **O4: decode sort and filter** (§3.8).
* **O5 and O6: the CQ's tries and tries on every queue row**, shipped 2026-10-14 ([autoseq.md](../architecture/autoseq.md), **Tries**; [webapp.md](../architecture/webapp.md), **The CQ row** and **Tries on a queue row**).
* **The Activation section and the add screen ship with RFC 0008's slices:**
  - A1 brings typed references.
  - A3 brings the store, the nearby list and search, and the map's pick mode.
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
* **Decode sort and filter** (§3.8): the default sort is named Default, since within a slot every decode has the same time and "Newest" sorted nothing; New waits on row `xv23`; POTA / SOTA means `CQ POTA` and `CQ SOTA` calls; a decode with no grid fails the distance filter.
* **CQ is a queue entry with a try budget.** Call CQ queues a CQ with N tries; each CQ sent uses one, an answer gives none back, and at zero it ends. Every queue entry's count can be changed in place. This replaces the beacon mode, O5's per-QSO retries and O6's CQ timeout. Built ([autoseq.md](../architecture/autoseq.md), **Tries**).
* **Call CQ picks its parity each time:** the active QSO's parity if there is one, otherwise the next slot it can still key. The CQ row shows it, and a tap flips it for that CQ, for a strong station nearby; SOTA spotting also wants the parity (RFC 0008 §3). Built (same).

**2026-10-14, building the CQ's tries:**

* **Every row counts tries, transmissions,** not retries. The Station key `tries` (default 6, the sends that 5 retries gave) replaces `autoseq_max_retry`.
* **One setting** bounds each QSO message and is what Call CQ starts with; there is no separate CQ count.
