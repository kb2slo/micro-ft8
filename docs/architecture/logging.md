# Logging: the ADIF record, and the four-place field chain

How a QSO becomes an ADIF record, how the record's fields cross Port A, and how the web app turns them back into a file, so that pota.app and the SOTA database accept it unedited. What is still planned for activations: epic [I18](../roadmap/activator-mode.md).

## The writer

`adif_log_entry()` (`components/qso_log/adif_logger.cpp`) is the one function that logs a completed QSO — `poe_p4/main/porta_host.cpp` builds an `AdifStationSnapshot` from `s_station` and hands it over. It:

* Expands `/Radio`/`/Grid` comment macros (`expand_comment_macros()`) against the station's configured radio name and grid.
* Upper-cases and unit-suffixes the configured band name for the ADIF `BAND` tag (`format_adif_band_tag()` — `"20m"` → `"20M"`), as POTA requires (**What POTA and SOTA require**).
* Splits `act_refs` (`Station.txt`'s config key, e.g. `POTA:US-1234,SOTA:W7O/CC-001` — `components/station/station.cpp`'s `normalize_act_refs()` is what actually validates and upper-cases it) into `MY_POTA_REF`/`MY_SOTA_REF` comma lists by program (`split_act_refs()`), each with its `"POTA:"`/`"SOTA:"` prefix stripped. Empty when there is no activation — no tag is written, matching the existing convention for an empty grid or comment.

`adif_format_log_record()` (`components/adif/adif.cpp:548`) is the pure formatter: fixed field order (`call, gridsquare, mode, qso_date, time_on, freq, band, station_callsign, my_gridsquare, rst_sent, rst_rcvd, comment, my_pota_ref, my_sota_ref`), each optional field omitted rather than written empty. `webapp/app/app.html`'s `buildAdifRecord()` (below) mirrors this order field-for-field, since the two must produce byte-compatible records for existing archives to merge cleanly.

**Not yet built** (epic [I18](../roadmap/activator-mode.md), **Later** and row [w5nj](../ROADMAP.md#w5nj)): `MY_GRIDSQUARE` still logs at 4 characters (`grid_ft8_4()`, `poe_p4/main/porta_host.cpp`), not the 6 or 8 ADIF allows. `MY_STATE` and the other station's reference (`POTA_REF`/`SOTA_REF`, for park-to-park and summit-to-summit) are not logged: neither has a data source yet.

## The four-place field chain

**Export is rebuilt, not copied.** `FILE_READ` never ships raw ADIF (`components/porta_proto/include/porta_proto.h`'s own reasoning); the browser rebuilds each line from parsed fields. A field the operator needs in an exported file has to exist in all four places, in order:

1. **The writer** — `adif_log_entry()`/`adif_format_log_record()` above.
2. **The reader** — `components/qso_log/qso_browse.cpp`'s `parse_record_line()` re-parses each stored ADIF line (tag-by-tag, honoring ADIF's own declared byte length) into a `QsoLogEntry`, for both the Log tab's display and export.
3. **The Port A wire row** — `porta_qso_entry_row_t` (`components/porta_proto/include/porta_proto.h`), a length-prefixed field list `porta_proto_encode_file_entry_row()`/`porta_proto_parse_file_entry_row()` (`components/porta_proto/porta_proto.c:880`/`:951`) carry over `FILE_READ`. Adding a field is a breaking wire change (`PORTA_PROTO_VERSION` bump, `porta_proto.h:63`'s own rule) since the field count is implicit in the byte stream's own order, not tagged.
4. **The JS builder** — `webapp/app/app.html`'s `buildAdifRecord()` (`app.html:3674`) rebuilds the ADIF line from the row's fields, in the same order as `adif_format_log_record()`.

Skipping any one of these means the field reaches the device's own log correctly but the export silently drops it — the four-place chain's whole failure mode is quiet, not a crash. `my_pota_ref`/`my_sota_ref` and `band` went through all four in one pass (2026-09-29, `PORTA_PROTO_VERSION` 2 → 3); `pota_ref`/`sota_ref` (the *other* station's reference, A5) were deliberately left out of the wire row for now — the four fields together would have pushed the row's worst case past `PORTA_PROTO_MAX_PAYLOAD` (255 bytes; `my_pota_ref`/`my_sota_ref` alone land the row at 221), and nothing reads them yet.

## The sidekick's `/api/log/entries` JSON

`sidekick/main/host_link.c`'s `file_entries_json_add()` is a fifth place in practice, though not part of the wire-format cost above: it re-escapes every `porta_qso_entry_row_t` field into the JSON `/api/log/entries` serves the browser. Its own comment explains why every field gets a `static` scratch buffer rather than a stack local: past a certain field count the function's own stack frame was enough to overflow the `porta_rx` task's 4 KB stack (field-found 2026-09-15).

## Export: the per-day archive and the n-fer split

`webapp/app/app.html`'s QSOs tab groups logged QSOs by day file. Two downloads exist per day:

* **"Download ADI"** (`downloadDayAdif()`) — the exact, unedited day, one file, every QSO, `MY_POTA_REF`/`MY_SOTA_REF` as the raw comma lists the device wrote. This is the truthful superset: one file with comma lists is not an *upload* format, because POTA does not read `MY_POTA_REF` (**Why not**), but it is the right shape for an archive.
* **The n-fer split**, triggered from the same link once the day has any activation reference. **N-fer expansion happens at export, not on the device.** `dayActivationRefs()` (`app.html:3704`) counts the day's distinct POTA/SOTA references (for the link's own text, e.g. "Download ADI (2 POTA, 1 SOTA)"); `groupRowsByReference()` (`app.html:3726`) buckets every row by each reference it carries — a QSO logged during two simultaneous activations (a 2-fer) lands in both buckets, since the device's own record is the superset. Each bucket becomes one file via `buildAdifRecord(q, dateStr, bucket)`'s `sig` argument, which replaces the raw comma lists with the one field format each program's real importer reads (**What POTA and SOTA require**): `MY_SIG`=`POTA`/`MY_SIG_INFO`=the park for a POTA bucket, `MY_SOTA_REF`=the summit for a SOTA bucket. Filenames follow PoLo's `CALL@PARK-YYYYMMDD.adi` (`exportFileBaseName()`, `app.html:3757`), a SOTA reference's own `/` swapped for `-` since it can't sit in a filename component. Downloads are staggered ~150 ms apart (`downloadDayNfer()`) — de-risked against real desktop and iOS Safari before this shipped, since a `Blob`+`<a download>` click firing several downloads at once is not guaranteed to land them all.
* **"Download full ADI"** (`downloadAllAdif()`) stays untouched by any of this — every day, concatenated, still the archive rather than a submission export.

**Not yet built:** a QSO's record carrying the *other* station's reference, and a line per partner park in the n-fer split (row [w5nj](../ROADMAP.md#w5nj)).

## CQ type follows the activation

Adding the first reference (`act_refs` empty → non-empty) switches `cq_type` on its own — SOTA if any SOTA reference is present (a mixed activation defaults to SOTA), else POTA. `porta_host_config_set()` detects exactly that one transition and set `cq_type` before applying the rest of the write, so the existing `sync_autoseq_station()` path picks it up with no extra plumbing. `act_refs_prefer_sota()` (`components/station/station.cpp`) is the one-line predicate. A later add or remove never re-derives, so it cannot clobber a manual switch made through the Station sheet's own Type control ([webapp.md](webapp.md), **The Station sheet**).

A TX already armed for the current slot picks up a switch: [autoseq.md](autoseq.md)'s head check before the trigger refreshes an armed TX's text in place when the queue head's identity has not changed.

## What POTA and SOTA require

External facts, checked 2026-09-26. They change without notice, so a change that depends on one re-checks it, and a real upload is the proof.

**POTA** ([ADIF for POTA](https://docs.pota.app/docs/activator_reference/ADIF_for_POTA_reference.html), [Park to Park](https://docs.pota.app/docs/activator_reference/park_2_park.html), [Digital Modes](https://docs.pota.app/docs/digital_modes.html), [Hunter Guide](https://docs.pota.app/docs/hunter_guide-english.html)):

* Required: `STATION_CALLSIGN` or `OPERATOR`, `CALL`, `QSO_DATE`, `TIME_ON` (QSO start), **`BAND`** with its unit (`20M`), `MODE`.
* Read: `MY_SIG`=`POTA` and `MY_SIG_INFO`=the park; `SIG`/`SIG_INFO` for park to park; `MY_STATE`. With no `MY_SIG_INFO`, the uploader takes the park from a file named `CALL@PARK.adi`.
* **N-fers are one file per park,** with a QSO line for every combination ("If you are two-fer and working a three-fer activator, you generate two ADIF log files, each having three lines"); without a distinct `SIG_INFO` on each line they are "rejected as duplicates".
* ADIF 3.1.4's `MY_POTA_REF`/`POTA_REF` accept comma lists, but POTA does not read them yet (Ham2K PoLo's `src/extensions/activities/pota/POTA Notes.md`); PoLo writes both, and so does our record.
* An activation is 10 QSOs in a UTC day. A contact is unique by call, band, mode and park; `/P` and `/M` count as different calls. P2P matching needs both logs within 15 minutes and exactly matching callsigns, so `STATION_CALLSIGN` stays exactly what was transmitted.

**SOTA** (K0NR, [Combined logging for SOTA and POTA](https://www.k0nr.com/wordpress/2022/03/logging-for-sota-and-pota/); the SOTA reflector): the database imports ADIF, with `MY_SOTA_REF` on every record of the activation and `SOTA_REF` on summit-to-summit contacts. An activation is 4 QSOs, from different stations; points count only from inside the activation zone, typically within 25 m vertical of the summit.

**ADIF 3.1.6** ([spec](https://adif.org/316/ADIF_316.htm)): `POTARef` is `xxxx-nnnnn[@yyyyyy]`; `MY_GRIDSQUARE` takes 2, 4, 6 or 8 characters.

**FT8 practice** (POTA's guides; the SOTA reflector, [summit identifying on FT8](https://reflector.sota.org.uk/t/sota-summit-identifying-on-ft8/25464), [long callsigns](https://reflector.sota.org.uk/t/newbie-activate-summit-on-ft8-with-long-callsign/38524)): activators send `CQ POTA <call> <grid>`; the reference does not fit the message, so hunters take it from spots, and POTA's hunter guide says to work a `CQ POTA` station even with the park unknown. SOTA advice is a non-standard frequency, spotted with the exact frequency, offset and parity. Compound calls (`EA4/G7WKX/P`) lose the grid, and two compound calls cannot complete a QSO.

## Why not

* **The reference in `COMMENT`,** the WSJT-X workaround POTA's digital guide describes: no importer reads it.
* **N-fers as one upload with comma lists:** POTA does not read `MY_POTA_REF`, so the per-park files it asks for are built at export.
* **Sending the reference over the air** as free text, after 73 or in place of the sign-off: hunters take the park from spots, and a free-text slot costs a 15 s cycle per QSO. Reopen if P2P and S2S capture from spots and queue entries (row [w5nj](../ROADMAP.md#w5nj)) proves not to be enough.
* **Raw ADIF over Port A for export:** it reverses `porta_proto.h`'s recorded reason for parsed rows, for a problem the row's free space covers. Reopen if the row runs out of room.
