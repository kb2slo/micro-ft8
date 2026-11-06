# Activator mode (epic I18)

POTA and SOTA from the field: the operator at a park or on a summit, others working them, the whole activation run offline and logged so pota.app and the SOTA database accept it unedited. Rows: [rrsp](#rrsp), [w5nj](#w5nj) ([ROADMAP.md](../ROADMAP.md)). Built so far: the activation's references, its log and the n-fer export ([logging.md](../architecture/logging.md)); the Station sheet's references, the progress line and the cues on decodes ([webapp.md](../architecture/webapp.md), **The Station sheet**, **Activation progress**, **Activation cues on decodes**).

## Goal

* **The activation runs offline.** It may be planned online beforehand; once in the field, choosing it, operating it and logging it need no internet ([system.md](../architecture/system.md), constraint 1).
* **The top goal, built: a log pota.app and the SOTA database accept without hand-editing** ([logging.md](../architecture/logging.md), **What POTA and SOTA require**). A real upload still waits on a real activation.
* **Then what helps an activator:** a nearby registered reference picked from a list or the map, a grid that matches where the operator is, planning (references near me, 2-fer candidates), and spotting.

## Constraints

* **Whatever must work offline lives on a device; the browser only caches.** Safari evicts browser storage, a second phone lacks it, and the phone may be asleep in a pocket while autoseq logs QSOs.
* **There is no mode.** An activation is the station having references; clearing them is the whole of ending one ([webapp.md](../architecture/webapp.md), **Why not, on Operate**). Each Operate surface is built for the home case first, and an activation adds references to a surface that already exists.
* **POTA and SOTA only** for now; nothing here should make WWFF or hunter mode harder.
* **The programs' rules change without notice,** so each child re-checks the ones it depends on ([logging.md](../architecture/logging.md), **What POTA and SOTA require**).

## Decided

* **The reference store lives on the sidekick, capped; the active references stay on the main board.** Every reader of the store is the browser, which the sidekick serves directly; the main board needs only `act_refs`.
* **The store is filled from the map's own POTA and SOTA fetches,** not a second download path.
* **No `/P` feature.** It is a regional convention, not an FT8 or program requirement, and POTA counts `K1ABC/P` as a different call from `K1ABC`, which complicates P2P matching and dupes. The own-call field is free text, so a suffixed call still logs faithfully. Reopen if an operator in a jurisdiction that expects it asks.
* **The reference stays off the air** ([logging.md](../architecture/logging.md), **Why not**).
* **The grid check ships with the store,** not with offline location: until references have a location there is nothing to compare, and Set from GPS sets the grid itself.
* **The SOTA API decision of 2026-09-21 covers bulk `summitslist.csv` downloads** from `storage.sota.org.uk` for planning ([webapp.md](../architecture/webapp.md), **Tools**).

## Design

### The reference store (Later: A3)

Saved areas of POTA parks and SOTA summits, each record carrying reference, program, trimmed name, lat/lon, grid and summit points, about 60 bytes.

* **Layout:** a reserved directory in the sidekick's `web` partition that no bundle ships or touches (for example `data/refs/`): one file per saved area, and an index of name, bounding box, date saved and count. Reads are open; saving and deleting need the pairing token.
* **Budget:** the partition is 1.9 MB; the bundle takes about 470 KB minified, and an install briefly stages a second copy. The store is capped (proposed 256 KB, about 4,000 references; the web build's budget assumes it, [webapp.md](../architecture/webapp.md), **Build**); the saved-areas list shows usage and refuses past the cap, saying why.
* **Filled by the map's existing fetches.** `map.html` already loads POTA parks per country (`api.pota.app/program/parks/{prefix}`) and SOTA summits per association region while online. **Save this area** filters what is loaded to the view and writes it. The live path and the store share one record format and one parser, so a stored area and a live fetch draw identically; offline, the store is the map's POTA/SOTA source.
* **Freshness:** each area shows its age; saving it again replaces it.
* **To confirm in code first:** that bundle commit never removes files outside its manifest, and what boot or rehydrate does to an unmountable filesystem. A format would lose the store, acceptable only because it can be downloaded again.
* **If the cap pinches:** the sidekick's OTA slots are 3 MB (`sidekick/partitions.csv`), about three times its image, so a repartition could roughly double `web`. It costs a USB reflash of every sidekick, so only alongside another one.
* **The browser caches** in IndexedDB, keyed by area and version, for speed only; it is never the only copy.

### Location, nearby and the grid check

Location, in order of preference:

1. **The phone's GNSS, offline** (row [rrsp](#rrsp)): a service worker on the HTTPS release host caches `geo.html`, so the hand-off works with no network, and a phone's GNSS needs no cell signal. It is the only phone-location path the P4 has offline.
2. **The selected reference's coordinates:** POTA's data carries a 6-character grid, SOTA gives coordinates. A suggestion only: a big park's point can be kilometres from the operator.
3. **Manual entry,** as today.

* **Nearby:** the stored extract sorted by distance from the fix (PoLo takes a 0.25° box, then sorts). The data is points, not boundaries, so the UI says "nearest registered references", never "you are in".
* **Typed references:** search covers the whole store, not only what is nearby. A well-formed reference not in the store still adds, marked **unverified** (no name, location or grid check); the next time the app is online it checks the reference against pota.app or the SOTA API and flags one that does not exist; the per-reference export warns about an unverified reference before upload. Format-only entry is built ([webapp.md](../architecture/webapp.md), **The Station sheet**).
* **The grid check:** a warning, never a block, when the configured grid, the fix's grid and the chosen references disagree, or a reference is further than a threshold. The operator can be right when the data is not.

### On Operate

Mockups, private to the operator: "Operate Dock Options" (https://claude.ai/artifact/HazeSpixV3UbDfUJDKqzGD), its `Add a park or summit`, `Add, offline: typing a reference` and three `Map ·` boards; "Operate Redesign" (https://claude.ai/artifact/GsMDrsrNLEtnUC7aVNxptF).

* **In the Station sheet:** an **unverified** badge on a reference the store does not hold; **Add a park or summit**; the grid check, on top of the sheet's format check.
* **The add screen,** from the Station sheet: search by reference or name across the whole store, with an All / POTA / SOTA filter; the nearby list with distance, bearing and summit points and an **Add / Added** toggle on each row, so an n-fer is a few taps; a **Saved for offline** card (what is saved, how old) with **Choose on the map ›**; a well-formed reference not in the store adds as unverified, malformed input gets the format hint; a standing caveat that distances are to each reference's listed point; **Done · N added** returns to the sheet.
* **The map in pick mode** (`map.html?pick=refs&lat=…&lon=…`, beside `?grid=` and `?lat=&lon=`): zoomed out, your pin with its accuracy ring and **Save this area for offline**, which draws the area's outline; zoomed in, labels on, a popup's **Add to activation**, an added one blue, **Done · N** returns; offline, only saved areas draw, and saving is disabled with the reason.
* **Changes to the map** ([webapp.md](../architecture/webapp.md), **The map**): parks as circles and summits as diamonds (both hollow circles today), added ones filled blue; the 40-marker cap never hides a reference near your pin; an outline layer for saved areas; the popup always offers **Add to activation**, however the map was opened.
* **Honest limit:** the basemap is Natural Earth 110 m, so at park zoom the background is nearly blank. Enough to pick by distance, not to judge "am I inside?".

### The partner's reference (row [w5nj](#w5nj))

A P2P or S2S contact counts for the hunter only if our log carries their reference. It reaches the record at log time from a live spot when online (`callinfo.js` already matches spots to callsigns), or from a reference the operator attaches to the queue entry, shown on its queue row. The record logs `POTA_REF`/`SOTA_REF`, and the n-fer export writes one `SIG_INFO` line per partner park ([logging.md](../architecture/logging.md), **What POTA and SOTA require**). Adding one *after* logging needs record editing (rows [I31](ideas.md#I31), [I32](ideas.md#I32)).

### Planning (Later: A6)

Online, with results stored for offline use: references near a point, near the phone or along a route, reusing `map.html`'s fetchers and layers; a saved plan stored as an extract (the store, above); **2-fer candidates** as a proximity list ("these references are within X km") for the operator to confirm, since POTA says no list of 2-fer locations exists; **scheduling** as deep links to pota.app's scheduler and SOTAwatch alerts, not API writes. A scheduled POTA activation is what lets RBN auto-spot the activator, an offline payoff ([QRPer](https://qrper.com/2022/04/how-to-use-the-reverse-beacon-network-rbn-for-automatic-pota-and-sota-spotting/); unverified for FT8 here).

### Spotting (Later)

* **POTA self-spotting** is an unauthenticated `POST https://api.pota.app/spot` (PoLo's `POTAPostSpotAPI.js`), but a write under the operator's name is a different risk class ([webapp.md](../architecture/webapp.md), **Tools**) and needs its own operator decision.
* **SOTAwatch** needs SSO and a registered User-Agent, and its API terms restrict AI-authored clients.
* **Offline,** pre-scheduling plus RBN is zero code here (RBNHole does the same for SOTA from alerts; FT8 coverage unverified). SOTAmat encodes a spot in FT8 free text that PSKReporter skimmers relay; its encoding is undocumented and needs its author's cooperation.

Prior art read for this design (read, not copied): Ham2K PoLo (offline park and summit lists refreshed monthly, nearest park from GPS, one export per reference), qFT8 (partner references back-filled from spots within an hour), FT8TW (lists cached offline, per-summit export).

## Children

* <a id="rrsp"></a>**rrsp: Set from GPS with no internet** (**Location, nearby and the grid check**, item 1). A service worker on the release host caches `geo.html`. **The probe:** `geo_handoff.js`'s reachability check would refuse a hand-off that works, so it is skipped for 7 days after a hand-off returns a fix (remembered in the app's `localStorage`), since WebKit may delete a site's service worker after 7 days unvisited; otherwise it probes as today, so an offline first use still says why. **Done-when:** in its row (field). **Lands in:** webapp.md (**A non-secure origin**, the cached hand-off); time.md if the grid's source is recorded there; TEST_PLAN Pending; site audit (Getting started, if it says Set from GPS needs internet).
* <a id="w5nj"></a>**w5nj: the partner's reference on queue rows and in the log.** **Blocked on** row [syb2](ideas.md#syb2): the QSO row has no room. **Done-when:** in its row. **Lands in:** logging.md (the record, the four-place chain, the export); webapp.md (the queue row); TEST_PLAN Pending (a P2P upload accepted by pota.app).

### Later

* **A3: the reference store,** the add screen, map pick mode, the offline map layer and the grid check (**The reference store**, **Location, nearby and the grid check**, **On Operate**). After it, a whole activation works offline.
* **A0's remainder: `MY_GRIDSQUARE` at full length** ([logging.md](../architecture/logging.md), **The writer**); `MY_STATE` once the store gives one.
* **A6: planning** (**Planning**).
* **Hunter mode** (row [hw2z](ideas.md#hw2z)): spots matched to decodes, `SIG_INFO` on the hunter's own log, worked-park tracking.
* **More programs:** WWFF (`MY_WWFF_REF`, 44 QSOs) and others PoLo supports.
* **Park boundaries** (PAD-US, as potamap.us uses) for real n-fer detection and an "inside the park" check.
* **Self-spotting** (**Spotting**).

## Open

* **The store's cap,** and whether areas are named and listed for deletion (proposed: yes, with usage shown). For A3.
* **The measured free space in `web`** on a device with the current bundle, which sets the cap. For A3.
* **Whether the phone's GNSS works offline on iOS** through the cached `geo.html`. For [rrsp](#rrsp), whose field check answers it.

## Why not

* **Full reference databases on the device.** POTA's `all_parks_ext.csv` is 9.4 MB and SOTA's `summitslist.csv` 25.2 MB (2026-09-26), against 3 MB of main-board storage; an activator needs only the references near where they are going.
* **The active activation in browser storage only:** evicted by Safari, invisible to a second phone, absent while the phone sleeps. The main board logs the QSO, so it holds the references.
* **The store on the main board:** a second hop over Port A and new protocol work for data only the browser reads.
