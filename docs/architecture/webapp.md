# Web app architecture

The web app (`webapp/app/`) is the station's only UI on the headless P4, served by the sidekick ([sidekick.md](sidekick.md)) and run in the phone's browser. System context: [system.md](system.md).

## Screens

**Operate / QSOs / Tools** are three swipe tabs (`app.html`, `#tabbar`); **Settings** is a full-screen overlay opened from the header gear. The test for a tab is whether the operator flips to it *mid-QSO*: "should I change band?" is such a question, so Tools is a tab; Settings is not, so it is an overlay.

**Every action reports where the operator is looking.** The page's status line sits under the header, which the overlay covers, so Settings repeats it on a line under its own fixed header. **Save station** closes Settings on success, leaving "Station saved." on Operate, and on a refusal stays open with the reason and the form reloaded to what the station holds. An action's result is never replaced by the form reload that follows it.

* **Operate** stacks a state header (slot countdown and parity, CAT and audio liveness, Connect/Tune), the queue (**The CQ row** below) and the decode stream, with the dock (**The dock** below, band and frequency) pinned above the tab bar.
* **Grow by destination, never by feature.** QRZ is not a screen: per-callsign data expands on the row it belongs to. One shared expand-on-tap component, used on decode, queue and QSO rows, shows **offline-derived** facts at once (DXCC and zones from the prefix, distance and bearing from grid arithmetic) and **online-enriched** ones (QRZ bio, photo) when reachable, failing legibly when not.
* **QSOs is one continuous list** with day dividers ("Today", "Sep 13"), not a day-file picker. The main board keeps one ADIF file per UTC day; the list loads the current day on open and older days as the operator scrolls (`FILE_LIST`/`FILE_READ`), because Port A's budget cannot carry the whole log on every open. A line under the toolbar shows the main board's log storage, "N MB free of M MB", from the live `storage_total_kb` / `storage_used_kb` config lines; it is hidden while the board has not said, and red under 10% free.
* **Worked/new flags are computed in the browser** from the QSOs loaded so far, for decode rows and the map. A station worked on a day not yet loaded shows as new; accepted 2026-09-25, and row xv23 explores loading and caching more of the log.

### The CQ row

The queue's last row, always present (`app.html`, `#opCqRow`), replacing the old header beacon control and the queue's own CQ/CALLING placeholder entry. It sits below the scrolling QSO rows, not inside them, so a long queue neither hides it nor loses rows to it.

* **Off | Even | Odd** is the beacon (`BeaconMode`, `POST /api/beacon`), labeled "CQ" here because that is what it transmits. Never a single tap-to-cycle button — Off → Even → Odd on one control could key the transmitter on a stray tap.
* **The Sends text** mirrors autoseq's `generate_cq_text_into()` (`components/autoseq/autoseq.cpp`): the type's prefix (`CQ`, `CQ SOTA`, `CQ POTA`, `CQ QRP`, `CQ FD`) plus the call and the grid's first four characters, or the free-text field verbatim for a Free-text CQ. Tapping it, or **Settings ›**, opens the Station sheet (**The Station sheet** below).
* **The status line** reads "Off"; "Calling · Even/Odd slots" when the beacon is on and the queue holds no other station; or "Waits for the queue · Even/Odd slots" once a real QSO is active, since every live QSO outranks a CQ ([autoseq.md](autoseq.md)).
* **Stop, during our own CQ, turns the beacon off too.** The TX HUD's cancel button reads **Stop CQ** while the CQ is what's on the air, and posts `/api/tx/cancel` then `/api/beacon 0`; during a QSO message it reads **Stop TX** and only cancels that transmission. Which one is decided from the text on the air (`txIsOurCq()`: it starts with `CQ `, or is a Free text CQ's `free_text`), never from the queue: the P4 arms the next beacon CQ during an empty cycle, and it stays queued behind a QSO that answers, while our report is what transmits. Otherwise the beacon stayed armed and the next CQ went out a cycle later regardless of the operator's tap.
* **The CQ decode color** (`--good-hot`, `.c-cq`) is `#007a00` in light mode, `#4ade80` in dark. The light value was `#00c800` (about 2.3:1 on white), which fails WCAG's 4.5:1 text-contrast minimum.

### The dock

Pinned above the tab bar and shown only on Operate (`app.html`, `#opDock`), so band stays one tap away however long the decode list grows. It replaces the old header's band select (`opBandSel`) and its "Next TX" offset line; the next TX's offset returns in the TX banner with O2 ([RFC 0009](../rfcs/0009-operate-station-surface.md) §3.12), and Connect and Tune stay in the header until then (§6).

* **Why it sits outside the Operate panel.** Each tab is its own scrollport (`#viewTrack > section`), and the three tabs slide on a track moved by a CSS transform, so neither a sticky nor a fixed element inside a panel stays on screen. The dock is a fixed sibling of the tab bar instead, hidden off Operate (`body.on-operate`). The page column ends at the tab bar, and `layoutDock()` shortens the Operate panel by the dock's height, which changes with a second band row or the open frequency editor.

* **The band grid** (`#opBandGrid`) shows `active_bands`, one button of at least 44 px per band. Up to seven bands share one row with columns equal to the count (`grid-template-columns: repeat(N, 1fr)`, `buildDockBandGrid()`); more than seven wrap to a second row on their own, so no band is ever a swipe away.
* **A tap changes band at once** (`changeBand()`), asking first only when a QSO is queued or our own transmission is on the air ("ends your QSO with K9ABC and stops the transmission on the air," `confirmRetune()`), then cancelling both (`cancelQsosAndTx()`) and writing the band's standard frequency and `band_sel` in one PUT. A "no" leaves the grid as it was — it never shows the new band until the write actually lands.
* **A band tap always lands on the band's standard frequency** (`standardKhz()`, the app's table), never the last frequency typed there (operator decision 2026-09-28), including a tap on the band already selected; one already at its standard sends nothing. A tap closes the frequency editor. A typed or nudged frequency therefore holds only until the next band tap. Keeping chosen frequencies is row `y9js`.
* **The dock's second line** reads "`<freq>` MHz ›" and "Retries `<N>` · `<offset>` · Station ›" (`renderDockLine()`). The frequency button opens the frequency editor above the band grid; the rest of the line opens the Station sheet (**The Station sheet** below). It previews the Station settings it opens, so the offset is the setting: "Random offset", "RX offset", or the fixed value in Hz (`offsetSettingLabel()`; operator decision 2026-09-28).
* **The frequency editor** (`#opFreqEditor`) opens from the frequency line and closes from its **✕**, Escape, a second tap on the line, or a change that goes ahead. A refusal or a declined confirmation leaves it open with the typed value.
  - A typed value under 1000 is MHz, otherwise kHz, kept to 0.1 kHz (the station file stores tenths).
  - **The keyboard's ✓ / Done acts like Set** (operator request 2026-09-28). On iOS it sends no key event, only closing the keyboard, so an edit committed with no tap just before it applies; a tap (Set, ✕, ±1, anywhere) reaches the page first and its own handler decides instead.
  - **Set**/Enter, **−1/+1 kHz**, and **Standard** (the app's own default for that band and protocol) all write the operating band's own key (`band<N>`, or `ft4_band<N>` under FT4) with one `putConfigLine()` PUT.
  - **A value in another active band always asks first** ("Change band to 20m? 14.080 MHz is on 20m.", plus what it would end; operator decision 2026-09-28), then cancels like `changeBand()` and sends that band's key and `band_sel` in one PUT, frequency first. A non-standard value, such as the calling frequency SOTA's FT8 advice asks for ([RFC 0008](../rfcs/0008-activator-mode.md) §3), holds until the next band tap; saving one to return to is row `y9js`.
  - A value outside every Region 2 amateur band, or on a band not in `active_bands`, is refused client-side (`bandForKhz()`) with the reason, in a dialog. The band edges (kHz, ITU Radio Regulations Article 5) are a sanity check, not a licence check; 60 m is one range spanning the US channels and the WRC-15 allocation:

    | Band | 160 | 80 | 60 | 40 | 30 | 20 | 17 | 15 | 12 | 10 | 6 | 2 |
    |---|---|---|---|---|---|---|---|---|---|---|---|---|
    | From | 1800 | 3500 | 5330 | 7000 | 10100 | 14000 | 18068 | 21000 | 24890 | 28000 | 50000 | 144000 |
    | To | 2000 | 4000 | 5407 | 7300 | 10150 | 14350 | 18168 | 21450 | 24990 | 29700 | 54000 | 148000 |

  - **A same-band change mid-QSO is treated like a band change** (operator decision 2026-09-28): the same confirmation, then the queued QSOs end and a transmission on the air stops before the new frequency is written. The partner is listening on the old frequency. Stopping the TX first also matters because a frequency the VFO cannot reach mid-TX is still saved, and nothing retunes the radio when the TX ends, so the dock would show a frequency the radio is not on. With nothing to end, no cancel is sent: a cancel also blocks a TX armed for the current slot.
* **Reply refuses a decode heard on another frequency** (`replyFrequencyMismatch()`; operator request 2026-09-28). A reply goes out on the frequency the radio is on now, so after a band or frequency change a decode from before it gets a dialog ("K1ABC was heard on 7.074 MHz, and the radio is on 14.074 MHz now…") and nothing is queued. The decode's own frequency comes from the main board (next bullet); the radio's now is the dock's. If either is unknown, the reply goes ahead.
* **The main board names each decode's slot and frequency** (Port A v2, [sidekick.md](sidekick.md), **Port A**). The app used to label a slot by what was current when its decodes arrived, a second or two after the slot ended, so headers missed the slot grid and a band change in that gap mislabelled a slot. Now each decode carries the slot it was recorded in (`si`, `sl`) and the dial frequency at the middle of its own signal (`fq`). The app groups decodes by slot (a new slot starts a new batch; SLOT_STATE's parity is only the fallback for a v1 host) and, within a slot, by frequency: a slot retuned partway through reads as two batches with the same time, each under the frequency its decodes were heard on, and nothing is dropped (operator decisions 2026-09-28). **A slot is timed at its end**, on the :00/:15/:30/:45 grid (7.5 s for FT4), so a batch that has just arrived reads "0 s ago", and its header shows its span, `2026-09-28 19:03:30–45z` (`fmtSlotSpan()`), so a WSJT-X-style start reads true too; its band is named from its frequency. **Reply is offered by the clock, not by position in the list** (`slotReplyOpen()`): only on a slot that ended less than two slot lengths ago, the current and previous slot (30 s on FT8, 15 s on FT4). Decodes shown after the app sat idle, restored from its cache or caught up from the sidekick, carry their real slot time and have no Reply however high they sit, and a tab that stayed open redraws the moment it is visible again. A station still calling reappears with its own button, and the main board remembers only its last 64 decodes for a reply, two or three slots on a busy band (operator decisions 2026-09-28). The QSO log is untouched by any of this. Covered by `webapp/tests/test_decode_batching.js`.
* **The main board refuses a frequency write that does not reach the radio.** `station_key_names_operating_band()` (`components/station/station.cpp`, host-tested) answers whether a CONFIG key names the band actually being operated — `band_sel`, or that band's own frequency key under the active protocol. `porta_host_config_set()` calls it where it already tested for `band_sel`, and refuses with `station_vfo_refusal()`'s text: "band saved, VFO not moved (TX active)" for `band_sel`, "frequency saved, …" for a frequency key, or "… (CAT not ready)". A write to any other band's frequency, or to `active_bands`, stays silent on a sync miss, same as before. The refused value is still saved: the refusal says the radio did not move, not that nothing changed. So after any refused write the app re-reads the station, and the dock shows what it holds; a refusal of a dock action also shows in a dialog, since the operator is looking at the dock, not the status line (`putConfigLine()`). With CAT down, the P4 retunes to the saved frequency when the radio reconnects.
* **Settings keeps only which bands appear** (`active_bands`, `buildBandRows()`). `band_sel` and the per-band frequency rows left Settings for the dock; Save no longer sends either key, which also ends the mid-TX `band_sel` refusal that could hit every Settings save (§2.1 of [RFC 0009](../rfcs/0009-operate-station-surface.md), now moot since Settings can no longer trigger it).

### The Station sheet

A full-screen overlay (`#stationSheet`, same slide-up mechanics as `#settingsOverlay`), opened from the CQ row's **Settings ›** and the dock's **Station ›**, and never open at the same time as Settings — each `open*()` closes the other first. Unlike Settings there is no Save button: **each control writes its own key the moment it changes**, with one `putConfigLine()` PUT, which reloads the whole station either way — a success brings the CQ row's Sends text and the dock line back in sync with what actually landed, and a refusal (shown in a dialog, since the operator is looking at this sheet, not the status line) reloads the control back to what the station still holds.

* **Calling CQ:**
  - A **Sends** preview, `cqSendsText()`, the same function the CQ row's own preview uses.
  - **Type** (`cq_type`, 0–5: CQ, SOTA, POTA, QRP, FD, FreeText).
  - **One free-text field, shown only for FD and FreeText,** writing `free_text` for both (`setStationSheetForm()`): the label switches between "FD exchange" and "CQ message", and only FreeText caps it at 13 characters (`maxlength`, removed for FD, which has no such limit). Since both types write the same key, switching from one to the other leaves the previous type's text sitting in the field until a new CQ actually goes out under the new type.
  - **Grid**, with a client-side format check (`gridFormatOk()`) matching `normalize_grid_maidenhead()` exactly (`components/station/station.cpp`): 4, 6 or 8 characters, field `A`–`R`, square `0`–`9`, subsquare `A`–`X`, extension `0`–`9`. A malformed grid is refused in a dialog and never sent — the firmware itself only silently *keeps the old value* on a bad write (`StationKey::Grid` in `station_parse()`) rather than reporting a refusal, so without this check a typo would read as accepted while the grid never changed. A note under the field always says the CQ carries the first four characters, plus the same compact location badge a decode or QSO row shows for a grid (`CallInfo.shortLocation()`, `callinfo.js` — "TX·US"), so typing a grid gives the same "is this where I think it is" check the operator already gets on someone else's. **Set from GPS** reuses the `geo.html` handoff (**A non-secure origin** below); its return route is `#stationsheet`, which `applyHashRoute()` opens straight to this sheet.
* **QSOs:** retries (`autoseq_max_retry`) and TX offset (`offset`, `offset_src`) — the same fields and validation Settings carried before this sheet existed.
* **Activation:** a program+reference list (`act_refs`, e.g. `POTA:US-1234,SOTA:W7O/CC-001`) with an Add row and an × per reference (RFC 0008 §4.2, §4.3 "Typed references, offline"). The program is inferred from the reference itself — a `/` means SOTA, otherwise POTA — and a reference containing `,` or `:` (the list's own delimiters) is refused client-side before it is ever sent, since it could never round-trip. Format-only: there is no store, nearby list, map, or verified/unverified badge yet, since nothing exists to check a reference against until A3's reference store ships (row `I18`). `components/station/station.cpp`'s `normalize_act_refs()` is the actual source of truth — it drops a malformed token on its own rather than rejecting the whole write, and is what a manual Station.txt edit or a future add-screen (A3) is also held to. **Adding the first reference (empty → non-empty `act_refs`) switches `cq_type` to `CQ POTA` or `CQ SOTA` on its own** (SOTA if any SOTA reference is present, a mixed activation defaults to SOTA), in `porta_host_config_set()` (RFC 0008 §4.4). Only that one transition derives it — adding or removing a reference afterward never overrides a manual Type change made through this same sheet.
* **Footer:** the call, read-only, with a note that it is changed in Settings — the call is the licence identity, not something changed mid-QSO.
* **Not in the sheet:** Off / Even / Odd (the queue row) and band/frequency (the dock) — each control has one home.
* **Settings' own Save dropped these keys** (`cq_type`, `free_text`, `grid`, `autoseq_max_retry`, `offset`, `offset_src`): its `FIELDS` array no longer lists them, so `formBody()` never sends them and an all-keys Settings save can no longer overwrite a value this sheet just wrote (§2.1 of [RFC 0009](../rfcs/0009-operate-station-surface.md)). `cq_ft` left the web app outright rather than moving here: it never reaches the air (`poe_p4/main/porta_host.cpp` sends `free_text` for a FreeText or FD CQ), so nothing needed its own control. It still exists in the station file; row `sfwz` retires it from there.
* **Not yet in the sheet:** CQ timeout (row `s6qk`) and the grid check against a station's references (row `I18`, RFC 0008 A4).

### Decisions a reference app would make differently

Surveyed 2026-09-14: WSJT-X, FT8CN, iFTx and the standalone touchscreen builds.

| Not imported | Why |
| --- | --- |
| Waterfall | Its control job in every reference app is picking the TX offset, and `resolve_tx_offset()` already does that without asking: Random re-rolls 500–2500 Hz per transmission, which decorrelates us from the stale gaps every operator taps. Spectrum data would also blow Port A's budget. Reopen if field operation shows repeated collisions re-rolling does not avoid. |
| Mode control (Listen / Call / Exchange) | Autoseq's priority queue already sequences: every live QSO outranks a CQ, so the operator turns the beacon on with a parity and replies take priority. A mode control would add an error state that does not exist. EVEN/ODD is the one Micro-FT8-specific control. |
| A single "current QSO" panel | The queue holds up to `AUTOSEQ_MAX_QUEUE` (30) concurrent QSOs, active and inactive ([autoseq.md](autoseq.md)); one panel would misrepresent it. The queue is a first-class region, and at 30 rows it simply scrolls. |
| A view per feature | Too much for a phone, and mid-QSO tab-switching runs against a 15-second clock. |
| Modifier-key interactions | There are no modifiers on a phone. |

Borrowed deliberately: FT8CN's lesson that the operator must see **which slot a tap lands in** (the countdown sits beside the tap target); iFTx's **worked-before colouring** on CQ rows; and WSJT-X's two information needs, "everything on the band" and "what concerns me", kept as **emphasis within one stream**, not two panes.

## A non-secure origin

`http://micro-ft8.local` is not a secure context, and that is a design choice, not an oversight: the alternative is a certificate for a name that resolves on one LAN. Measured 2026-09-21 in Chromium (Safari gates these APIs on secure contexts the same way); the last row follows from the spec rather than a measurement:

| API | Here |
| --- | --- |
| `localStorage`, IndexedDB, `fetch` to HTTPS | work |
| Cache API, Service Worker | **absent** |
| `navigator.geolocation` | property present, **call refused** (feature detection lies) |
| Web Serial, Web Bluetooth, Web Push | absent |

**So "cache it in the browser for offline" does not exist on this origin.** Anything that must work offline is served by the sidekick from its `web` partition, or it is not available offline.

**Grid from the phone's location** therefore needs a real secure context: `webapp/geo.html` on the release host (HTTPS). The app does a **top-level navigation** there, the page calls `navigator.geolocation`, and it redirects back with the grid. An HTTPS iframe does not work: secure-context checks walk the whole ancestor chain. Reached from Settings' Grid field and the map's locate button; both confirm before overwriting an existing grid. The sidekick is not a party to the exchange.

## Tools

What the operator consults *instead of* looking at the radio. **The test for a Tools card: if it expands an existing row, it is not a Tools card** — a worked-before flag, a DXCC name or a QRZ bio stays on its row. Cards are questions the operator asks, not vendors we integrate.

**Every card declares a tier and degrades visibly:**

* **Offline-derived** — computed from data the app already has; works on a summit with cellular off.
* **Online-optional** — offline-derived, enriched when reachable; complete without the enrichment.
* **Online-required** — meaningless without the internet, and says so when it cannot reach its source.

**A card never shows a stale value as a current one.** A four-hour-old K-index shown as now is worse than an empty card, because the operator makes a band decision on it: stale data carries its age or is not shown.

**Sources** are called directly when they send CORS, and a Worker route is added only when they do not ([system.md](system.md), **Third-party services**). Payload size is part of the choice: a phone on cellular must not pay for a large file to show one number.

* **Space weather** (`app.html`): NOAA SWPC's small current-value endpoints (10.7 cm flux, X-ray flare class, solar wind speed and Bz/Bt, the R/S/G scales) and the planetary K-index, whose `a_running` supplies the A-index. Not hamqsl.com: no CORS and no JSON, and its per-band verdict is an undocumented formula (row B80 adds it through a Worker). Not NOAA's aurora file (922 KB) or sunspot history (no small endpoint).
* **POTA and SOTA** (`map.html`): `api.pota.app` activator spots and `api-db2.sota.org.uk/api/spots/{n}/all/all`, both CORS-open. SOTA's API terms restrict AI-authored clients without prior approval; the operator chose to build against it anyway (2026-09-21).
* **Not in Tools:** self-spotting to POTA/SOTA — an authenticated outward write is a different risk class from a read-only card.

## The SWR sweep

`swr.html`, reached from Tools ([radio.md](radio.md), **The SWR sweep on the main board**): tune an antenna in the field. The bands ticked are swept by the main board through the QMX, one short carrier per point; the page plots what comes back. The CAT sequence and what the radio refuses: [radio.md](radio.md) (**QMX SWR Tune over CAT**); the wire: [sidekick.md](sidekick.md) (**Port A**, v4).

* **The plan is the page's.** `bands.js` (shared with `app.html`, which used to hold the table inline) gives the band edges; `swr_core.js` turns the ticked bands into `(lo, hi, step)` ranges, about 10 to 20 points a band (the step table is there), **Fine** halves every step. The ranges go to `POST /api/swr/start`. The host's three numbers (800 ms a point, 200 points, 12 ranges) are read from `swr_sweep.h` by `webapp/tests/test_swr_core.js`, so the estimate and the refusal cannot drift from the firmware. 2 m is left out: its edges do not fit the wire's 16-bit kHz.
* **Bands default to `active_bands`.** The ticked boxes open as Settings has them (`defaultBands()`), and only bands a QMX could have are offered.
* **The estimate and the countdown.** "About 1:14 (93 points)" before a run; while it runs the page counts the points still to do at the rate measured so far (the estimate until two have arrived), and a band the radio reports it lacks leaves the count.
* **One plot** (`plotModel()`, drawn as SVG): frequency across the swept bands only, each as wide as its span with a floor so a 50 kHz band is not a sliver, a vertical line at every band boundary; SWR from 1 to 6 with a dashed line at 2. **No point is drawn where there is no measurement, and the line breaks there.** A reading above 6 sits on the top edge as a red triangle. A band the radio does not have is labelled **not swept**, so a missing band never reads as a pass. Each band's minimum is named, and a table under the plot gives each band's lowest SWR, where, and the share of points at or below 2.
* **The previous run** is kept in `localStorage` (`microft8_swr_prev_v1`, readings only) and drawn faintly behind the next: run, adjust the wire, run again. A per-phone convenience, never the record.
* **Landscape is recommended, not required.** A page cannot force an orientation (iOS Safari has no `screen.orientation.lock()`), so in portrait a line says "Rotate your phone for a wider plot" and the plot draws at the width it has. Rotating re-lays out the readings already held; it never restarts a sweep (tested).
* **Starting asks first** when it would end something: the host refuses over a queued QSO, the beacon, a transmission or Tune, and the page then asks in the words of a band change ("A sweep ends your QSO with K9ABC, the beacon. Receive is also interrupted while it runs. Continue?"), ends them through the existing endpoints (`/api/beacon`, `/api/tx/cancel`, `/api/queue/cancel`, `/api/radio/tune`) and starts again. **Stop** is `/api/tx/cancel`.
* **Events are polled**, so a phone that sleeps through a sweep misses points; they are shown as missing, never as zero. The page polls every 400 ms while a sweep runs.
* **Run against a simulator:** `python3 tools/serve_webapp_local.py --sim` serves the page with `sim_station` behind it, which runs the real sweep against a fake QMX that reads like a 40 m end-fed half-wave (bands 40 to 10 m; 160, 80, 60 and 6 m refused).

## The map

`map.html`, reached from Tools: a full-screen map with independent layers (QSOs, decodes, POTA/SOTA spots) over a bundled basemap. **Only QSOs is on when the map opens**; every other layer starts off, and a layer that starts off fetches nothing until it is tapped on (operator decision 2026-10-08).

* **A vector basemap, not tiles.** Natural Earth 110m files (land, countries, lakes, US states, places; public domain) are bundled in `webapp/app/` and served by the sidekick, so the map works offline. Tiles cannot be cached on this origin, so a tile map would be blank exactly where the radio must work; and what the map shows (grid squares, station pins, great-circle paths) gains nothing from roads or imagery. Coordinates are rounded to 2 decimal places (~1 km), and each file stays under the 512 KB per-file limit.
* **Hand-rolled canvas, no framework.** Equirectangular projection with **one scale for both axes, cropping rather than distorting**: the base scale is `max(w/360, h/180)`, so the view always fills the canvas, like CSS `background-size: cover`, and never zooms out past it. Pan by drag, zoom by pinch (anchored at the midpoint) or wheel; longitude wraps at the antimeridian. Great-circle paths use spherical interpolation and split where they cross the antimeridian.

## Build

`webapp/app/` holds the hand-edited sources; what Pages serves and the sidekick installs is their build, `webapp/dist/app/`, produced by `webapp/tools/build.py`. Pages, CI's Host tests and `desk_push.py` all run it, so what is tested is what ships. `desk_push.py --usb` installs the same build with no network: `webapp/tools/usb_push.py` packs it into a LittleFS image (the firmware's block size, `name_max` 64 and 512 KB file cap) and `esptool` writes it to the `web` partition. It replaces the whole filesystem and leaves NVS alone; the next boot re-hydrates the seed pages. Needs `pip install littlefs-python`. Neither the output nor its manifest is committed: a committed manifest had to be regenerated by hand after every edit, and it was forgotten twice.

In order, the build:

1. copies the sources;
2. stamps the build id (`<short sha>[-dirty] <UTC date>`, dirty meaning `webapp/app/` has uncommitted changes) over `app.html`'s one `__APP_BUILD__`. Settings' **App** line shows it beside the FNV-1a self-hash, and an unbuilt page shows `source`;
3. minifies every `.html` and `.js` (`minify.mjs`: html-minifier-terser and terser, versions pinned in `webapp/tools/package-lock.json`);
4. compile-checks every script with `node:vm`, compacts the JSON data files, and writes `manifest.json` from the output;
5. fails if a file exceeds the sidekick's 512 KB per-file limit or the bundle exceeds 800 KB.

**Minification is conservative on purpose.** Top-level names are never renamed: the pages share globals across files (`CallInfo` from `callinfo.js`, `MicroFT8Pairing` from the sidekick's `/pairing.js`), and a readable top-level name is what makes an error on a device traceable to the source. Whitespace collapses to one space, never to none. A file the minifier would grow ships as it is. The first build took the bundle from 637 KB to 469 KB (73%).

**The 800 KB budget** comes from the `web` partition (1,920 KB). An install stages a second copy of the bundle before renaming it into place ([sidekick.md](sidekick.md), **Installing the app bundle**), and the reference store's proposed cap is 256 KB ([RFC 0008](../rfcs/0008-activator-mode.md) §4.2): 2 × 800 + 256 leaves 64 KB for the bootstrap pages and LittleFS overhead. Raising the budget means shrinking the store cap.

**Previewing:** `tools/serve_webapp_local.py` serves the sources at `/app/`; with `--dist` it serves the last build there instead, which is how a minifier problem shows up before a push. Without `--sim` there is no sidekick behind it, so the pages load and render but no API call succeeds; a bench check still needs `desk_push.py`. `--sim` proxies `/api/` to `host_mock/sim_station`, which plays `host_mock/test_qso.json` (or `--scenario`) on a real 15 s clock (`--scale` divides that wait) and serves the same event JSON the sidekick does. Open `http://127.0.0.1:8787/app/app.html`. The simulator's pairing window stays open, so the page pairs without a button press. That is a scripted station, not a radio: USB, CAT, and audio timing are not what it exercises.
