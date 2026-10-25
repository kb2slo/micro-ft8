# RFC 0008: Activator mode — POTA and SOTA from the field

* **Status:** Accepted 2026-09-27 (drafted 2026-09-26). Slices: §7. Decisions and deferred questions: §9. **Amended 2026-10-02:** the ADV was dropped (row za6n). Where this RFC discusses the ADV, its measurements, its screens or its `ADV-only` work, it is history and no longer applies.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** the activation as an object the station knows about; where it lives so an activation runs with no internet; what goes on the air; what the log records and how it is exported for pota.app and the SOTA database; choosing a nearby reference and checking the grid offline; planning activations while online; where spotting fits.
* **Does not cover:** how any of it looks on Operate ([RFC 0009](0009-operate-station-surface.md): this RFC supplies the data, 0009 the surface), hunter mode (§6), programs other than POTA and SOTA (§6), log editing and the on-device storage format ([ROADMAP.md](../ROADMAP.md) I31, I32).
* **Replaces** the design sketch in row I18.

---

## 1. Goal

Stated by the operator, 2026-09-26:

* **Activator mode.** The operator is at a park or on a summit and others work them. A **hunter mode** may follow; it is not designed here, and nothing here should make it harder.
* **The activation must run offline.** A planned activation may be prepared online beforehand. Once in the field, selecting it, operating it and logging it need no internet. This is [system.md](../architecture/system.md) constraint 1 applied to the feature.
* **The top goal: a log that pota.app and the SOTA database accept without hand-editing.**
* **Then, what helps an activator:** picking a nearby registered reference, making sure the grid matches where the operator actually is, planning activations (references near me while travelling, spotting 2-fers), and spotting (wanted, not required this round).
* **Go broad here, then slice** (§7).

## 2. What exists

Each fact is from the code or docs at `9d0d704`.

* **CQ.** `AutoseqCqType::SOTA` / `POTA` send `CQ SOTA <call> <grid>` / `CQ POTA <call> <grid>` (`components/autoseq/autoseq.cpp:788`). The grid in the CQ is 4 characters. That is the whole existing feature: a CQ prefix.
* **The log record.** `adif_format_log_record()` writes `call, gridsquare, mode, qso_date, time_on, freq, station_callsign, my_gridsquare, rst_sent, rst_rcvd, comment` (`components/adif/adif.cpp:547`). There is **no `BAND`**, and **no `MY_SIG` / `SIG` family**. `my_gridsquare` is cut to 4 characters (`grid_ft8_4(g_grid)`, `main/main.cpp:902`) even when the operator entered 6.
* **One ADIF file per UTC day** on the main board. That matches POTA, whose activation window is the UTC day.
* **Export is rebuilt, not copied.** `FILE_READ` sends parsed rows (`porta_qso_entry_row_t`, `components/porta_proto/include/porta_proto.h:486`), and the browser rebuilds each ADIF line from them (`buildAdifRecord`, `webapp/app/app.html:2856`). This was deliberate (`porta_proto.h:441`). The consequence: **a new logged field touches four places**, namely the writer, `qso_browse` parsing, the Port A row and the JS builder, or export silently drops it. The row already carries `band`, but export does not write it. The row's worst case today is about 140 bytes of the 255-byte payload.
* **Location.** The P4 has no GPS ([main_board.md](../architecture/main_board.md)). The ADV reads GNSS from the LoRa cap ([time.md](../architecture/time.md)). The browser can use geolocation only through `geo.html` on the HTTPS release host ([webapp.md](../architecture/webapp.md), **A non-secure origin**), so **today the phone's location needs internet**.
* **Browser storage on this origin.** `localStorage` and IndexedDB work; the Cache API and service workers do not ([webapp.md](../architecture/webapp.md)). Safari evicts script-writable storage after seven days unused ([system.md](../architecture/system.md)).
* **Reference data already fetched online.** `map.html` loads POTA parks per country (`api.pota.app/program/parks/{prefix}`) and SOTA summits per association region, driven by the viewport; `callinfo.js` and `map.html` read live activator spots. All of it is online-only today.
* **Device storage.** Sidekick `web` partition: LittleFS, 1.9 MB, 512 KB per file, holding the app bundle. Main boards: `storage` 3 MB LittleFS. Port A: 2.5 Mbaud, 255-byte payloads ([sidekick.md](../architecture/sidekick.md), **Port A**).
* **`/P`.** `ft8_lib` packs a `/P` suffix as message type i3=2, grid included (`components/ft8_lib/vendor/ft8/message.c:288`).
* **No reference goes on the air today.** An upstream convention for sending one in place of the sign-off is documented but has no code, and Backlog row `nuvm` removes its doc.

## 3. What the programs require

External facts, checked 2026-09-26. They change without notice, so every slice that depends on one re-checks it and finishes with a real upload (§7).

**POTA** ([ADIF for POTA](https://docs.pota.app/docs/activator_reference/ADIF_for_POTA_reference.html), [Park to Park](https://docs.pota.app/docs/activator_reference/park_2_park.html)):

* Required: `STATION_CALLSIGN` or `OPERATOR`, `CALL`, `QSO_DATE`, `TIME_ON` (QSO start), **`BAND`** (with unit, `20M`), `MODE`.
* Read by POTA: `MY_SIG`=`POTA`, `MY_SIG_INFO`=park; `SIG`/`SIG_INFO` for park-to-park; `MY_STATE`. If `MY_SIG_INFO` is missing, the uploader takes the park from a file named `CALL@PARK.adi`.
* **N-fers are one file per park**, with a QSO line for every combination: "If you are two-fer and working a three-fer activator, you generate two ADIF log files, each having three lines." Without a distinct `SIG_INFO` on each line, the combinations are "rejected as duplicates".
* ADIF 3.1.4 added `MY_POTA_REF` / `POTA_REF`, which accept comma lists. PoLo's notes say POTA does not read them yet (`src/extensions/activities/pota/POTA Notes.md`), and PoLo writes both.
* Activation: 10 QSOs in a UTC day. A contact is unique by call, band, mode and park; `/P` and `/M` count as different calls. P2P matching needs both logs within 15 minutes and exactly matching callsigns.

**SOTA** (SOTA reflector threads and K0NR's logging notes, §8):

* The SOTA database imports ADIF. `MY_SOTA_REF` goes on every record of the activation, and `SOTA_REF` on summit-to-summit contacts.
* Activation: 4 QSOs. Points count only from inside the activation zone, typically within 25 m vertical of the summit.

**ADIF 3.1.6:** `POTARef` is `xxxx-nnnnn[@yyyyyy]`. `MY_GRIDSQUARE` takes 2, 4, 6 or 8 characters.

**FT8 field practice** (§8):

* POTA activators send `CQ POTA <call> <grid>`. The park reference does not fit in the message, so hunters get it from spots, and POTA's own hunter guide says to work a `CQ POTA` station even when the park is unknown.
* SOTA advice: use a non-standard frequency, and spot with the exact frequency plus offset and parity. Compound calls (`EA4/G7WKX/P`) lose the grid, and two compound calls cannot complete a QSO.
* Sending the reference: as free text after 73 (M1EYP), or as tagged free text in place of the sign-off (AG6AQ).

**Spotting:**

* POTA's `POST https://api.pota.app/spot` takes unauthenticated JSON (`activator, spotter, frequency, reference, mode, source, comments`; PoLo `POTAPostSpotAPI.js`).
* SOTAwatch needs an SSO login and a registered User-Agent, and its API terms restrict AI-authored clients ([webapp.md](../architecture/webapp.md) records the operator's 2026-09-21 decision to read from it anyway).
* **Offline self-spotting already exists in two forms.**
  - **RBN auto-spot:** POTA spots a *scheduled* activation when the Reverse Beacon Network hears the activator's call. Activators report this working for FT8 `CQ POTA`; we have not verified it. SOTA's RBNHole does the same from SOTAwatch alerts; its FT8 coverage is unverified.
  - **SOTAmat:** encodes a spot in FT8 free text that PSKReporter skimmers relay. The encoding is undocumented and needs a registered account.

## 4. Design

### 4.1 The activation

One object, generic across programs so WWFF and hunter mode can reuse it:

* **References:** 1..N entries of program plus reference (`POTA US-1234`, `POTA US-5678`, `SOTA W7O/CC-001`). A summit inside a park is a normal mixed case.
* **On-air call:** the station call as configured today. Activator mode adds no `/P` option (§9).
* **Grid:** 6 characters.
* **`MY_STATE`:** optional; POTA's location descriptor supplies it (`US-ME`).
* **State:** *planned*, then *active*, then *ended*. Only one activation is active at a time. No activation active means today's behaviour, unchanged.

### 4.2 Where it lives

The rule: **whatever must work offline lives on a device; the browser only caches.** The browser cannot be the durable store: Safari evicts it, a second phone lacks it, and the phone may be asleep in a pocket while autoseq logs QSOs.

* **The active activation: main-board config.** Built as A1 ([webapp.md](../architecture/webapp.md), **The Station sheet**): `act_refs=POTA:US-1234,SOTA:W7O/CC-001` on the existing `CONFIG_*`/`Station.txt` path (`PORTA_CONFIG_VALUE_MAX`, 200 bytes), shared by both boards.
* **The reference store: the sidekick** (decided 2026-09-27). It holds saved areas of POTA parks and SOTA summits, each record carrying reference, program, trimmed name, lat/lon, grid and summit points, about 60 bytes.
  - **Why the sidekick.** Every reader is the browser (the map, the nearby list, search), and the sidekick serves the browser directly over WiFi. The main board would force every read through a second hop over Port A, plus new protocol messages, for data only the browser uses. The main board needs only the active references.
  - **Layout.** A reserved directory in the `web` partition that no bundle ships or touches (for example `data/refs/`). It holds one file per saved area plus an index of name, bounding box, date saved and count. Reads are open; saving and deleting need the pairing token ([system.md](../architecture/system.md), **Who may key the transmitter**).
  - **Budget.** The partition is 1.9 MB. The bundle takes about 470 KB since the build minifies it (650 KB of source; [webapp.md](../architecture/webapp.md), **Build**), and a bundle install briefly stages a second copy. The store is capped (for example 256 KB, about 4,000 references); the saved-areas list shows usage and refuses, with a clear message, past the cap.
  - **Filled by the map's existing fetches** (operator, 2026-09-27). `map.html` already downloads POTA parks per country and SOTA summits per association region while online (its "All" layers). **Save this area** filters what is already loaded to the view and writes it; there is no second download path. The map's live path and the store share one record format and one parser, so a stored area and a live fetch draw identically. Offline, the store is the map's POTA/SOTA source.
  - **Freshness.** Each area shows its age; saving an area again replaces it.
  - **To confirm in code before building:** that bundle commit never removes files outside its manifest, and what boot or rehydrate does to an unmountable filesystem. A format would lose the store, which is acceptable only because it can be re-downloaded.
  - **If the cap pinches:** the sidekick's OTA slots are 3 MB for a 955 KB image (RFC 0007 §2), so a repartition could roughly double `web`. It costs a USB reflash of every sidekick, so do it only alongside another one.
* **The browser caches** in IndexedDB, keyed by area and version, purely for speed. It is never the only copy.
* **Why not the main board for the store:** a second hop over Port A and new protocol work for data only the browser reads. An earlier draft chose the main board and rejected the sidekick for lack of a partition to add. Files in the existing `web` partition need no new partition.

### 4.3 Location and the grid check

In order of preference:

1. **The phone's GNSS, offline.** The release host is HTTPS, so it may run a service worker. One that caches `geo.html` makes the existing top-level navigation work with no network, and a phone's GNSS needs no cell signal. It is the only phone-location path the P4 has offline. **Unverified on iOS**, so the slice ends in a field check.
2. **The ADV's GNSS cap**, where fitted.
3. **The selected reference's coordinates.** POTA's data carries a 6-character grid, and SOTA gives coordinates. This is a suggestion only: a big park's point can be many kilometres from the operator.
4. **Manual entry**, as today.

**Nearby:** sort the stored extract by distance from the fix (PoLo takes a 0.25° box, then sorts). **The data is points, not boundaries**, so "nearest" does not mean "inside". The UI says "nearest registered references", never "you are in". The list and the grid check live in the station sheet ([RFC 0009](0009-operate-station-surface.md) §3.5 and §3.9).

**Typed references, offline:**

* **Search covers the whole store,** not only what is nearby, so a reference saved for another trip is found.
* **A well-formed reference not in the store can still be added.** The program comes from the format: a slash means SOTA (`W2/GC-001`), otherwise POTA (`US-1234`).
  - It is marked **unverified**: there is no name, no location and no grid check.
  - The next time the app is online it checks the reference against pota.app or the SOTA API, and flags one that does not exist.
  - The per-reference export (§4.5) warns about an unverified reference before upload.
* **Input that is not a reference** gets the format hint instead of an Add row.

**Grid check:** warn when the configured grid, the fix, and the chosen references disagree, i.e. the fix's grid differs from the configured one, or a reference is further than a threshold. A warning, never a block: the operator can be right when the data is not.

### 4.4 On the air

Built as A1 ([logging.md](../architecture/logging.md), **CQ type follows the activation**; [autoseq.md](../architecture/autoseq.md)): `CQ POTA`/`CQ SOTA` unchanged in shape, derived from `act_refs` on activation start (a mixed activation defaults to `CQ SOTA`, decided 2026-09-26); the Station sheet's Type control switches it live; a TX already armed for the current slot picks up a switch made after arming. **No `/P` feature** (decided 2026-09-26, §9): the own-call field is free text with no validation, so an operator can still enter a suffixed call and the log records it faithfully. **The reference stays off the air**, matching POTA practice: hunters get it from spots (§3); see §5 for why.

Still to add:

* **The other station's reference (P2P, S2S)** reaching the record at log time from a live spot when online (`callinfo.js` already matches spots to callsigns), or from a reference the operator attaches to the queue entry (A5). Adding one *after* logging needs record editing, which waits on I31/I32.

### 4.5 The log

Built as A1 ([logging.md](../architecture/logging.md)): one record per QSO, carrying `MY_POTA_REF`/`MY_SOTA_REF` as comma lists; n-fer expansion at export, not on the device — `MY_SIG`/`MY_SIG_INFO` per POTA file, `MY_SOTA_REF` per SOTA file, named `CALL@PARK-YYYYMMDD.adi` (PoLo's shape, `adifFieldCombinationsForOneQSO`); `BAND` on every record. The four-place cost (§2) and the field budget are in that doc.

Still to add:

* **The other station's reference** (`POTA_REF`/`SOTA_REF` for park-to-park/summit-to-summit) on the QSO record, and a distinct `SIG_INFO` line per partner park in the n-fer split, as POTA requires (A5).
* **`MY_GRIDSQUARE` at full length** — still cut to 4 characters ([logging.md](../architecture/logging.md)), a leftover from the still-open A0 slice, not part of A1.
* **`MY_STATE`** when known — no data source until A3's reference store ships.
* `STATION_CALLSIGN` already stays exactly what was transmitted (needed for POTA's exact-callsign P2P matching, §3) — unchanged, nothing to add.

### 4.6 Activation status

Offline-derived. It answers a mid-QSO question ("am I there yet?"), so it belongs on Operate, as the dock's progress line, and only while the station has references (decided 2026-09-26). Where and how it appears: [RFC 0009](0009-operate-station-surface.md) §3.10. What it computes:

* QSOs counted per reference against its threshold (POTA 10, SOTA 4).
* Unique by call, band and mode for a park; by call alone for a summit, since SOTA wants four different stations.
* A countdown to 0000Z, when a POTA activation starts over.
* A dupe cue on decode rows.

### 4.7 Planning

Online-required, with the results stored for offline use:

* Find references near a point, near the phone, or along a route, reusing `map.html`'s fetchers and layers.
* Save a plan, and store its extract on the device (§4.2).
* **N-fer candidates:** from point data this can only be a proximity list ("these refs are within X km"), labelled as candidates for the operator to confirm. POTA itself says no list of 2-fer locations exists. Park boundaries (PAD-US, as potamap.us uses) are a later phase.
* **Scheduling:** deep links to pota.app's scheduler and SOTAwatch alerts, not API writes. This has an offline payoff: a scheduled POTA activation is what lets RBN auto-spot the activator (§3).

### 4.8 Spotting

A later phase (§6). What the design keeps open:

* Online POTA self-spotting is an unauthenticated POST. [webapp.md](../architecture/webapp.md) still records it as a different risk class, a write under the operator's name, so it needs its own operator decision.
* SOTAwatch needs SSO and runs into the terms question.
* Offline, **pre-scheduling plus RBN** is zero code on our side. SOTAmat would need its author's cooperation for the encoding.

## 5. Rejected alternatives

* **Full reference databases on the device** (I18's sketch). POTA's `all_parks_ext.csv` is 9.4 MB and SOTA's `summitslist.csv` 25.2 MB (measured 2026-09-26), against 3 MB of main-board storage. An activator needs the references near where they are going, so an extract made at planning time is enough.
* **The active activation in browser storage only.** Evicted by Safari, invisible to a second phone, and absent while the phone sleeps. The main board logs the QSO, so the main board holds the references.
* **References in `COMMENT`**, the WSJT-X workaround POTA's digital guide describes. No importer reads it; it defeats the top goal.
* **N-fers as one file with comma lists.** POTA does not read `MY_POTA_REF` (§3), so the per-park files POTA asks for are built at export.
* **Sending the reference over the air** as free text, whether after 73 or in place of the sign-off. The one written convention for this is being dropped (row `nuvm`). In practice POTA hunters take the park from spots, and a free-text slot costs a 15 s cycle per QSO. Reopen if P2P/S2S capture from spots and queue entries (§4.4) proves not to be enough.
* **Raw ADIF over Port A for export.** Reverses a recorded decision (`porta_proto.h:452`) for a problem the row's free space covers. Reopen if the row runs out of room.

## 6. Later phases

Not part of this design; each gets a row or an RFC when wanted.

* **Hunter mode:** spots matched to decodes (started in `callinfo.js`), `SIG_INFO` on the hunter's own log, worked-park tracking.
* **More programs:** WWFF (`MY_WWFF_REF`; 44 QSOs), and others PoLo supports.
* **Park boundaries** for real n-fer detection and an "inside the park" check.
* **Self-spotting:** POTA POST; SOTAwatch; SOTAmat if its encoding becomes available.
* **Log editing** of references after the fact: I31, blocked on I32.

## 7. Slices

Accepted 2026-09-27. The slices stay listed here, under row I18, until each is next; then it becomes its own row.

**Order with RFC 0009** (agreed 2026-09-27): 0009 builds each Operate surface for the home case first, and this RFC then adds references to a surface that already exists, so no activation work ships its own screen. The firmware half of A1 does not wait on O1; only its entry in the sheet does.

1. **A0: log baseline.** `BAND` in records and export shipped as part of A1 below; `MY_GRIDSQUARE` at full length is still open (row `I18`).
2. **O0:** done 2026-09-27; RFC 0009 accepted.
3. **O1:** the dock layout, home case, in three pieces: the CQ row, the dock, and the Station sheet (RFC 0009 §6).
4. **A1: the activation and the log**, shipped 2026-09-29 ([logging.md](../architecture/logging.md)). **The top goal lands here.** Proof: `host_mock` and `webapp/tests/test_nfer_export.js` validate a record's fields and the export's n-fer expansion against the POTA/SOTA ADIF spec (§8) — a real pota.app/SOTA-database upload waits for an actual activation, per the operator's 2026-09-28 relaxation of this slice's Done-when. Field checks on the shipped mechanics (TX-arm timing, the wire version bump, the sheet's Activation section) are tracked in `TEST_PLAN.md` Pending, not this Done-when.
5. **A2: activation status** (§4.6), as the dock's progress line. Row `xe2j`.
6. **A4: offline phone location** (the service-worker `geo.html`, §4.3). Row `rrsp`. Field check: on an iPhone with cellular off, the grid fills from GNSS.
7. **A3: the reference store on the sidekick, filled from the map's fetches** (§4.2). Also the map's Save this area, pick mode and offline layer, the nearby list, search and unverified typed references in the add screen, and the grid check in the sheet (§4.3; the UI is [RFC 0009](0009-operate-station-surface.md)'s). After this, **a whole activation works offline**.
8. **A5: P2P/S2S capture:** a partner reference on queue entries, auto-filled from spots, and the "worked on this reference today" cue (§4.4).
9. **A6: the planning UI:** nearby, route, n-fer candidates, scheduling links (§4.7).

RFC 0009's O2–O4 (radio status, reply feedback, decode sort and filter) do not depend on this RFC and fit wherever they are most wanted.

## 8. Prior art

| Source | What we take from it |
|---|---|
| [POTA: ADIF for POTA](https://docs.pota.app/docs/activator_reference/ADIF_for_POTA_reference.html), [Park to Park](https://docs.pota.app/docs/activator_reference/park_2_park.html), [Digital Modes](https://docs.pota.app/docs/digital_modes.html), [Hunter Guide](https://docs.pota.app/docs/hunter_guide-english.html) | Required fields, n-fer file shape, P2P matching, FT8 practice |
| [ADIF 3.1.6](https://adif.org/316/ADIF_316.htm) | `POTA_REF`/`SOTA_REF` families, `POTARef` format, grid lengths |
| Ham2K PoLo ([polo.ham2k.com](https://polo.ham2k.com/), `github.com/ham2k/app-polo`, MPL-2.0; read, not copied) | Offline park list from `all_parks_ext.csv` refreshed monthly, SOTA from `summitslist.csv`; nearest park from GPS; one export per reference with field combinations; the POTA spot POST |
| qFT8 ([manual](https://qft8.com/manual/)): Android FT8 for QMX and others | `CQ POTA` modifier; `MY_SIG`/`MY_SIG_INFO`/`MY_POTA_REF` written; partner references back-filled from spots within an hour |
| FT8TW ([releases](https://github.com/danleetw/FT8TW/releases)) | Summit and park set in config, written on every QSO; lists cached offline; per-summit export |
| [SOTAmat](https://sotamat.com/faq/), [HROB: off-grid self-spotting](https://hamradiooutsidethebox.ca/2023/08/30/how-to-self-spot-off-grid-for-sota-pota-using-hf/) | Offline self-spot through FT8 free text and PSKReporter |
| [QRPer: RBN auto-spotting](https://qrper.com/2022/04/how-to-use-the-reverse-beacon-network-rbn-for-automatic-pota-and-sota-spotting/), [RBNHole](https://github.com/vk3arr/rbnhole) | Scheduling ahead enables auto-spotting from the field |
| SOTA reflector: [SOTA summit identifying on FT8](https://reflector.sota.org.uk/t/sota-summit-identifying-on-ft8/25464), [FT8 from summits](https://reflector.sota.org.uk/t/ft8-from-summits/33719), [long callsigns](https://reflector.sota.org.uk/t/newbie-activate-summit-on-ft8-with-long-callsign/38524), [QMX + Mini-FT8](https://reflector.sota.org.uk/t/qmx-mini-ft8-for-sota-activations/40042) | Reference as free text after 73 (M1EYP) or in place of the sign-off (AG6AQ), non-standard frequency and spotting advice (K6CPR), the compound-call limits; Mini-FT8 already in SOTA use |
| K0NR, [Combined logging for SOTA and POTA](https://www.k0nr.com/wordpress/2022/03/logging-for-sota-and-pota/) | `MY_SOTA_REF` / `SOTA_REF` in SOTA database uploads |
| [potamap.us](https://potamap.us/), [KB9VBR on stacking parks](https://www.jpole-antenna.com/2026/03/06/stacking-up-pota-parks-for-maximum-effect/) | Boundaries come from PAD-US; no 2-fer list exists |

## 9. Decisions and open questions

Decided by the operator, 2026-09-26:

1. **The ADV gets no activator UI of its own.** Activator mode is phone-driven on both boards. The ADV still logs the references correctly from `act_refs`, since the log is written on the main board (§4.2).
2. **A mixed POTA + SOTA activation defaults to `CQ SOTA`,** and the operator can switch while operating (§4.4).
3. **No `/P` support.** It is a regional convention, not an FT8 or program requirement: the US never required it, and POTA counts `K1ABC/P` as a different call from `K1ABC`, which complicates P2P matching and dupe counts. Received `/P` calls already decode and log as sent. Reopen if an operator in a jurisdiction that expects it asks.
4. **The 2026-09-21 SOTA API decision covers bulk `summitslist.csv` downloads** from `storage.sota.org.uk` for planning.

5. **The reference store lives on the sidekick, capped; the active references stay on the main board** (2026-09-27, §4.2). The store is filled from the map's own POTA/SOTA fetches.
6. **The grid check ships with A3, not A4** (2026-10-24, §7). Until the store gives references a location it has nothing to compare: the phone's fix arrives only through Set from GPS, which sets the grid itself.
7. **A summit counts different calls; a park counts calls unique by band and mode** (2026-10-24, §4.6). The earlier text gave POTA's rule for both.

Open, deferred at acceptance (2026-09-27) to the slice that needs each:

* **A0:** whether pota.app accepts `FREQ` without `BAND`. Moot once A0 lands, but it decides whether existing logs need re-export.
* **A3:** the store's cap, and whether areas are named and listed for deletion (proposed: yes, with usage shown).
* **A3:** the measured free space in the sidekick's `web` partition on a device with the current bundle, which sets the cap (§4.2). The web build's 800 KB budget assumes a 256 KB cap ([webapp.md](../architecture/webapp.md), **Build**).
