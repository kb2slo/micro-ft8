# Web app architecture

The web app (`webapp/app/`) is the station's only UI on the headless P4, served by the sidekick ([sidekick.md](sidekick.md)) and run in the phone's browser. System context: [system.md](system.md).

## Screens

**Operate / QSOs / Tools** are three swipe tabs (`app.html`, `#tabbar`); **Settings** is a full-screen overlay opened from the header gear. The test for a tab is whether the operator flips to it *mid-QSO*: "should I change band?" is such a question, so Tools is a tab; Settings is not, so it is an overlay.

**Every action reports where the operator is looking.** The page's status line sits under the header, which the overlay covers, so Settings repeats it on a line under its own fixed header. **Save station** closes Settings on success, leaving "Station saved." on Operate, and on a refusal stays open with the reason and the form reloaded to what the station holds. An action's result is never replaced by the form reload that follows it.

* **Operate** stacks a state header (slot countdown and parity, CAT and audio liveness, Connect/Tune), the queue (**The CQ row** below) and the decode stream, with the dock (**The dock** below, band and frequency) pinned above the tab bar. The ADV splits these across RX, TX and STATUS because 240×135 forces it; a phone shows all four at once, and that is the real gain.
* **Grow by destination, never by feature.** QRZ is not a screen: per-callsign data expands on the row it belongs to. One shared expand-on-tap component, used on decode, queue and QSO rows, shows **offline-derived** facts at once (DXCC and zones from the prefix, distance and bearing from grid arithmetic) and **online-enriched** ones (QRZ bio, photo) when reachable, failing legibly when not.
* **QSOs is one continuous list** with day dividers ("Today", "Sep 13"), not a day-file picker. The main board keeps one ADIF file per UTC day; the list loads the current day on open and older days as the operator scrolls (`FILE_LIST`/`FILE_READ`), because Port A's budget cannot carry the whole log on every open.
* **Worked/new flags are computed in the browser** from the QSOs loaded so far, for decode rows and the map. A station worked on a day not yet loaded shows as new; accepted 2026-09-25, and row xv23 explores loading and caching more of the log.

### The CQ row

The queue's last row, always present (`app.html`, `#opCqRow`), replacing the old header beacon control and the queue's own CQ/CALLING placeholder entry. It sits below the scrolling QSO rows, not inside them, so a long queue neither hides it nor loses rows to it.

* **Off | Even | Odd** is the beacon (`BeaconMode`, `POST /api/beacon`), labeled "CQ" here because that is what it transmits. Never a single tap-to-cycle button — Off → Even → Odd on one control could key the transmitter on a stray tap.
* **The Sends text** mirrors autoseq's `generate_cq_text_into()` (`components/autoseq/autoseq.cpp`): the type's prefix (`CQ`, `CQ SOTA`, `CQ POTA`, `CQ QRP`, `CQ FD`) plus the call and the grid's first four characters, or the free-text field verbatim for a Free-text CQ. Tapping it, or **Settings ›**, opens Settings today (**not yet built: row cuca** opens the Station sheet instead).
* **The status line** reads "Off"; "Calling · Even/Odd slots" when the beacon is on and the queue holds no other station; or "Waits for the queue · Even/Odd slots" once a real QSO is active, since every live QSO outranks a CQ ([autoseq.md](autoseq.md)).
* **Stop, during our own CQ, turns the beacon off too.** The TX HUD's cancel button reads **Stop CQ** while the CQ is what's on the air, and posts `/api/tx/cancel` then `/api/beacon 0`; during a QSO message it reads **Stop TX** and only cancels that transmission. Which one is decided from the text on the air (`txIsOurCq()`: it starts with `CQ `, or is a Free text CQ's `free_text`), never from the queue: the P4 arms the next beacon CQ during an empty cycle, and it stays queued behind a QSO that answers, while our report is what transmits. Otherwise the beacon stayed armed and the next CQ went out a cycle later regardless of the operator's tap.
* **The CQ decode color** (`--good-hot`, `.c-cq`) is `#007a00` in light mode, `#4ade80` in dark. The light value was `#00c800` (about 2.3:1 on white), which fails WCAG's 4.5:1 text-contrast minimum.

### The dock

Pinned above the tab bar and shown only on Operate (`app.html`, `#opDock`), so band stays one tap away however long the decode list grows. It replaces the old header's band select (`opBandSel`) and its "Next TX" offset line; the next TX's offset returns in the TX banner with O2 ([RFC 0009](../rfcs/0009-operate-station-surface.md) §3.12), and Connect and Tune stay in the header until then (§6).

* **Why it sits outside the Operate panel.** The page scrolls as a whole, and the three tabs slide on a track moved by a CSS transform, so neither a sticky nor a fixed element inside the panel stays on screen. The dock is a fixed sibling of the tab bar instead, hidden off Operate (`body.on-operate`), and `layoutDock()` keeps Operate's bottom padding equal to the tab bar plus the dock, whose height changes with a second band row or the open frequency editor.

* **The band grid** (`#opBandGrid`) shows `active_bands`, one button of at least 44 px per band. Up to seven bands share one row with columns equal to the count (`grid-template-columns: repeat(N, 1fr)`, `buildDockBandGrid()`); more than seven wrap to a second row on their own, so no band is ever a swipe away.
* **A tap changes band at once** (`changeBand()`), asking first only when a QSO is queued or our own transmission is on the air ("ends your QSO with K9ABC and stops the transmission on the air," `confirmRetune()`), then cancelling both (`cancelQsosAndTx()`) and writing the band's standard frequency and `band_sel` in one PUT. A "no" leaves the grid as it was — it never shows the new band until the write actually lands.
* **A band tap always lands on the band's standard frequency** (`standardKhz()`, the app's table), never the last frequency typed there (operator decision 2026-09-28), including a tap on the band already selected; one already at its standard sends nothing. A tap closes the frequency editor. A typed or nudged frequency therefore holds only until the next band tap. The band's key is the same one the ADV's BAND menu (`B`) edits, so a tap overwrites a frequency programmed there; keeping chosen frequencies is row `y9js`.
* **The dock's second line** reads "`<freq>` MHz ›" and "Retries `<N>` · `<offset>` · Station ›" (`renderDockLine()`). The frequency button opens the frequency editor above the band grid; the rest of the line opens Settings until the Station sheet ships (row `cuca`). It previews the Station settings it opens, so the offset is the setting: "Random offset", "RX offset", or the fixed value in Hz (`offsetSettingLabel()`; operator decision 2026-09-28).
* **The frequency editor** (`#opFreqEditor`) opens from the frequency line and closes from its **✕**, Escape, a second tap on the line, or a change that goes ahead. A refusal or a declined confirmation leaves it open with the typed value.
  - A typed value under 1000 is MHz, otherwise kHz, kept to 0.1 kHz (the station file stores tenths).
  - **The keyboard's ✓ / Done acts like Set** (operator request 2026-09-28). On iOS it sends no key event, only closing the keyboard, so an edit committed with no tap just before it applies; a tap (Set, ✕, ±1, anywhere) reaches the page first and its own handler decides instead.
  - **Set**/Enter, **−1/+1 kHz**, and **Standard** (the app's own default for that band and protocol) all write the operating band's own key (`band<N>`, or `ft4_band<N>` under FT4) with one `putConfigLine()` PUT.
  - **A value in another active band always asks first** ("Change band to 20m? 14.080 MHz is on 20m.", plus what it would end; operator decision 2026-09-28), then cancels like `changeBand()` and sends that band's key and `band_sel` in one PUT, frequency first. One round trip leaves no gap for a stale read between two writes (the sidekick's reply race, row `hvz8`). A non-standard value, such as the calling frequency SOTA's FT8 advice asks for ([RFC 0008](../rfcs/0008-activator-mode.md) §3), holds until the next band tap; saving one to return to is row `y9js`.
  - A value outside every Region 2 amateur band, or on a band not in `active_bands`, is refused client-side (`bandForKhz()`) with the reason, in a dialog. The band edges (kHz, ITU Radio Regulations Article 5) are a sanity check, not a licence check; 60 m is one range spanning the US channels and the WRC-15 allocation:

    | Band | 160 | 80 | 60 | 40 | 30 | 20 | 17 | 15 | 12 | 10 | 6 | 2 |
    |---|---|---|---|---|---|---|---|---|---|---|---|---|
    | From | 1800 | 3500 | 5330 | 7000 | 10100 | 14000 | 18068 | 21000 | 24890 | 28000 | 50000 | 144000 |
    | To | 2000 | 4000 | 5407 | 7300 | 10150 | 14350 | 18168 | 21450 | 24990 | 29700 | 54000 | 148000 |

  - **A same-band change mid-QSO is treated like a band change** (operator decision 2026-09-28): the same confirmation, then the queued QSOs end and a transmission on the air stops before the new frequency is written. The partner is listening on the old frequency. Stopping the TX first also matters because a frequency the VFO cannot reach mid-TX is still saved, and nothing retunes the radio when the TX ends, so the dock would show a frequency the radio is not on. With nothing to end, no cancel is sent: a cancel also blocks a TX armed for the current slot.
* **Reply refuses a decode heard on another frequency** (`replyFrequencyMismatch()`; operator request 2026-09-28). A reply goes out on the frequency the radio is on now, so after a band or frequency change a decode from before it gets a dialog ("K1ABC was heard on 7.074 MHz, and the radio is on 14.074 MHz now…") and nothing is queued. Both sides are the main board's `freq_khz` in whole kHz: the slot's, captured when its decodes arrived, and the latest config's; if either is unknown the reply goes ahead.
* **The main board refuses a frequency write that does not reach the radio.** `station_key_names_operating_band()` (`main/storage/station.cpp`, host-tested) answers whether a CONFIG key names the band actually being operated — `band_sel`, or that band's own frequency key under the active protocol. Both boards call it where they already tested for `band_sel`, and refuse with `station_vfo_refusal()`'s text: "band saved, VFO not moved (TX active)" for `band_sel`, "frequency saved, …" for a frequency key, or "… (CAT not ready)". A write to any other band's frequency, or to `active_bands`, stays silent on a sync miss, same as before. The refused value is still saved: the refusal says the radio did not move, not that nothing changed. So after any refused write the app re-reads the station, and the dock shows what it holds; a refusal of a dock action also shows in a dialog, since the operator is looking at the dock, not the status line (`putConfigLine()`). With CAT down, the P4 retunes to the saved frequency when the radio reconnects.
* **Settings keeps only which bands appear** (`active_bands`, `buildBandRows()`). `band_sel` and the per-band frequency rows left Settings for the dock; Save no longer sends either key, which also ends the mid-TX `band_sel` refusal that could hit every Settings save (§2.1 of [RFC 0009](../rfcs/0009-operate-station-surface.md), now moot since Settings can no longer trigger it).

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

## The map

`map.html`, reached from Tools: a full-screen map with independent layers (QSOs, decodes, POTA/SOTA spots) over a bundled basemap.

* **A vector basemap, not tiles.** Natural Earth 110m files (land, countries, lakes, US states, places; public domain) are bundled in `webapp/app/` and served by the sidekick, so the map works offline. Tiles cannot be cached on this origin, so a tile map would be blank exactly where the radio must work; and what the map shows (grid squares, station pins, great-circle paths) gains nothing from roads or imagery. Coordinates are rounded to 2 decimal places (~1 km), and each file stays under the 512 KB per-file limit.
* **Hand-rolled canvas, no framework.** Equirectangular projection with **one scale for both axes, cropping rather than distorting**: the base scale is `max(w/360, h/180)`, so the view always fills the canvas, like CSS `background-size: cover`, and never zooms out past it. Pan by drag, zoom by pinch (anchored at the midpoint) or wheel; longitude wraps at the antimeridian. Great-circle paths use spherical interpolation and split where they cross the antimeridian.

## Build

`webapp/app/` holds the hand-edited sources; what Pages serves and the sidekick installs is their build, `webapp/dist/app/`, produced by `webapp/tools/build.py`. Pages, CI's Host tests and `desk_push.py` all run it, so what is tested is what ships. Neither the output nor its manifest is committed: a committed manifest had to be regenerated by hand after every edit, and it was forgotten twice.

In order, the build:

1. copies the sources;
2. stamps the build id (`<short sha>[-dirty] <UTC date>`, dirty meaning `webapp/app/` has uncommitted changes) over `app.html`'s one `__APP_BUILD__`. Settings' **App** line shows it beside the FNV-1a self-hash, and an unbuilt page shows `source`;
3. minifies every `.html` and `.js` (`minify.mjs`: html-minifier-terser and terser, versions pinned in `webapp/tools/package-lock.json`);
4. compile-checks every script with `node:vm`, compacts the JSON data files, and writes `manifest.json` from the output;
5. fails if a file exceeds the sidekick's 512 KB per-file limit or the bundle exceeds 800 KB.

**Minification is conservative on purpose.** Top-level names are never renamed: the pages share globals across files (`CallInfo` from `callinfo.js`, `MicroFT8Pairing` from the sidekick's `/pairing.js`), and a readable top-level name is what makes an error on a device traceable to the source. Whitespace collapses to one space, never to none. A file the minifier would grow ships as it is. The first build took the bundle from 637 KB to 469 KB (73%).

**The 800 KB budget** comes from the `web` partition (1,920 KB). An install stages a second copy of the bundle before renaming it into place ([sidekick.md](sidekick.md), **Installing the app bundle**), and the reference store's proposed cap is 256 KB ([RFC 0008](../rfcs/0008-activator-mode.md) §4.2): 2 × 800 + 256 leaves 64 KB for the bootstrap pages and LittleFS overhead. Raising the budget means shrinking the store cap.

**Previewing:** `tools/serve_webapp_local.py` serves the sources at `/app/`; with `--dist` it serves the last build there instead, which is how a minifier problem shows up before a push. Neither has a sidekick behind it, so the pages load and render but no API call succeeds; a bench check still needs `desk_push.py`.
