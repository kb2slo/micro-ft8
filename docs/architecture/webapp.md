# Web app architecture

The web app (`webapp/app/`) is the station's only UI on the headless P4, served by the sidekick ([sidekick.md](sidekick.md)) and run in the phone's browser. System context: [system.md](system.md).

## Screens

**Operate / QSOs / Tools** are three swipe tabs (`app.html`, `#tabbar`); **Settings** is a full-screen overlay opened from the header gear. The test for a tab is whether the operator flips to it *mid-QSO*: "should I change band?" is such a question, so Tools is a tab; Settings is not, so it is an overlay.

**Every action reports where the operator is looking.** The page's status line sits under the header, which the overlay covers, so Settings repeats it on a line under its own fixed header. **Save station** closes Settings on success, leaving "Station saved." on Operate, and on a refusal stays open with the reason and the form reloaded to what the station holds. An action's result is never replaced by the form reload that follows it.

* **Operate** stacks a state header (slot countdown and parity, beacon, band and frequency, CAT and audio liveness, resolved TX offset), the queue, and the decode stream. The ADV splits these across RX, TX and STATUS because 240×135 forces it; a phone shows all three at once, and that is the real gain.
* **Grow by destination, never by feature.** QRZ is not a screen: per-callsign data expands on the row it belongs to. One shared expand-on-tap component, used on decode, queue and QSO rows, shows **offline-derived** facts at once (DXCC and zones from the prefix, distance and bearing from grid arithmetic) and **online-enriched** ones (QRZ bio, photo) when reachable, failing legibly when not.
* **QSOs is one continuous list** with day dividers ("Today", "Sep 13"), not a day-file picker. The main board keeps one ADIF file per UTC day; the list loads the current day on open and older days as the operator scrolls (`FILE_LIST`/`FILE_READ`), because Port A's budget cannot carry the whole log on every open.
* **Worked/new flags are computed in the browser** from the QSOs loaded so far, for decode rows and the map. A station worked on a day not yet loaded shows as new; accepted 2026-09-25, and row xv23 explores loading and caching more of the log.

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
