# Logging: the ADIF record, and the four-place field chain

How a QSO becomes an ADIF record, how the record's fields cross Port A, and how the web app turns them back into a file. [RFC 0008](../rfcs/0008-activator-mode.md) is the design history; this doc is how it works now.

## The writer

`adif_log_entry()` (`main/storage/adif_logger.cpp:159`) is the one function both boards call to log a completed QSO — `main/main.cpp:898` and `poe_p4/main/porta_host_stubs.cpp:183` each build an `AdifStationSnapshot` from their own live state (loose globals on the ADV, `s_station` on the P4 — [autoseq.md](autoseq.md)'s station-model split applies here too) and hand it to the shared function. It:

* Expands `/Radio`/`/Grid` comment macros (`expand_comment_macros()`, `adif_logger.cpp:21`) against the station's configured radio name and grid, on **both** boards — before this was shared (fixed 2026-09-28, row `j4xh`), the P4's own copy of this logic skipped macro expansion entirely, so a P4-logged QSO kept the literal `/Radio`/`/Grid` text.
* Upper-cases and unit-suffixes the configured band name for the ADIF `BAND` tag (`format_adif_band_tag()`, `adif_logger.cpp:41` — `"20m"` → `"20M"`), added 2026-09-29 to close the gap RFC 0008 §2 found: `BAND` reached the Port A row (derived from `<freq>`) but was never written by the logger or read back by the export.
* Splits `act_refs` (`Station.txt`'s config key, e.g. `POTA:US-1234,SOTA:W7O/CC-001` — `main/storage/station.cpp:103`'s `normalize_act_refs()` is what actually validates and upper-cases it) into `MY_POTA_REF`/`MY_SOTA_REF` comma lists by program (`split_act_refs()`, `adif_logger.cpp:57`), each with its `"POTA:"`/`"SOTA:"` prefix stripped. Empty when there is no activation — no tag is written, matching the existing convention for an empty grid or comment.

`adif_format_log_record()` (`components/adif/adif.cpp:548`) is the pure formatter: fixed field order (`call, gridsquare, mode, qso_date, time_on, freq, band, station_callsign, my_gridsquare, rst_sent, rst_rcvd, comment, my_pota_ref, my_sota_ref`), each optional field omitted rather than written empty. `webapp/app/app.html`'s `buildAdifRecord()` (below) mirrors this order field-for-field, since the two must produce byte-compatible records for existing archives to merge cleanly.

**Not yet built:** `MY_GRIDSQUARE` still logs at 4 characters (`grid_ft8_4()`, both boards), not the full 6–8 characters RFC 0008 §4.5 calls for — a leftover gap in the still-open "A0: log baseline" slice ([ROADMAP.md](../ROADMAP.md) row `I18`), separate from and not closed by A1's work above. `MY_STATE` and the other station's own reference (`POTA_REF`/`SOTA_REF`, for park-to-park/summit-to-summit) are deliberately not logged — no data source exists for either until A3's reference store and A5's partner-reference capture ship (row `I18`).

## The four-place field chain

RFC 0008 §2 named this cost when it found `BAND` reaching the wire but never the export: **export is rebuilt, not copied.** `FILE_READ` never ships raw ADIF (`components/porta_proto/include/porta_proto.h:441`'s own reasoning); the browser rebuilds each line from parsed fields. A field the operator needs in an exported file has to exist in all four places, in order:

1. **The writer** — `adif_log_entry()`/`adif_format_log_record()` above.
2. **The reader** — `main/storage/qso_browse.cpp`'s `parse_record_line()` re-parses each stored ADIF line (tag-by-tag, honoring ADIF's own declared byte length) into a `QsoLogEntry`, for both the Log tab's display and export.
3. **The Port A wire row** — `porta_qso_entry_row_t` (`components/porta_proto/include/porta_proto.h`), a length-prefixed field list `porta_proto_encode_file_entry_row()`/`porta_proto_parse_file_entry_row()` (`components/porta_proto/porta_proto.c:880`/`:951`) carry over `FILE_READ`. Adding a field is a breaking wire change (`PORTA_PROTO_VERSION` bump) since the field count is implicit in the byte stream's own order, not tagged.
4. **The JS builder** — `webapp/app/app.html`'s `buildAdifRecord()` (`app.html:3674`) rebuilds the ADIF line from the row's fields, in the same order as `adif_format_log_record()`.

Skipping any one of these means the field reaches the device's own log correctly but the export silently drops it — the four-place chain's whole failure mode is quiet, not a crash. `my_pota_ref`/`my_sota_ref` and `band` went through all four in one pass (2026-09-29); `pota_ref`/`sota_ref` (the *other* station's reference, A5) were deliberately left out of the wire row for now — the four fields together would have pushed the row's worst case past `PORTA_PROTO_MAX_PAYLOAD` (255 bytes; `my_pota_ref`/`my_sota_ref` alone land the row at 221), and nothing reads them yet.

## The sidekick's `/api/log/entries` JSON

`sidekick/main/host_link.c`'s `file_entries_json_add()` is a fifth place in practice, though not part of the wire-format cost above: it re-escapes every `porta_qso_entry_row_t` field into the JSON `/api/log/entries` serves the browser. Its own comment explains why every field gets a `static` scratch buffer rather than a stack local: past a certain field count the function's own stack frame was enough to overflow the `porta_rx` task's 4 KB stack (field-found 2026-09-15).

## Export: the per-day archive and the n-fer split

`webapp/app/app.html`'s QSOs tab groups logged QSOs by day file. Two downloads exist per day:

* **"Download ADI"** (`downloadDayAdif()`) — the exact, unedited day, one file, every QSO, `MY_POTA_REF`/`MY_SOTA_REF` as the raw comma lists the device wrote. This is the truthful superset RFC 0008 §5 calls for ("N-fers as one file with comma lists" was rejected as the *upload* format precisely because POTA does not read it — but it is still the right shape for an archive).
* **The n-fer split**, triggered from the same link once the day has any activation reference. RFC 0008 §4.5: *n-fer expansion happens at export, not on the device.* `dayActivationRefs()` (`app.html:3704`) counts the day's distinct POTA/SOTA references (for the link's own text, e.g. "Download ADI (2 POTA, 1 SOTA)"); `groupRowsByReference()` (`app.html:3726`) buckets every row by each reference it carries — a QSO logged during two simultaneous activations (a 2-fer) lands in both buckets, since the device's own record is the superset. Each bucket becomes one file via `buildAdifRecord(q, dateStr, bucket)`'s `sig` argument, which replaces the raw comma lists with the one field format each program's real importer reads (confirmed against pota.app's importer and Ham2K PoLo's own notes, RFC 0008 §3): `MY_SIG`=`POTA`/`MY_SIG_INFO`=the park for a POTA bucket, `MY_SOTA_REF`=the summit for a SOTA bucket. Filenames follow §4.5's `CALL@PARK-YYYYMMDD.adi` (`exportFileBaseName()`, `app.html:3757`), a SOTA reference's own `/` swapped for `-` since it can't sit in a filename component. Downloads are staggered ~150 ms apart (`downloadDayNfer()`) — de-risked against real desktop and iOS Safari before this shipped, since a `Blob`+`<a download>` click firing several downloads at once is not guaranteed to land them all.
* **"Download full ADI"** (`downloadAllAdif()`) stays untouched by any of this — every day, concatenated, still the archive rather than a submission export.

**Not yet built:** the P2P/S2S half of §4.5 — a QSO's own record carrying the *other* station's reference, and per-partner-park lines in the n-fer split — waits on A5 (partner-reference capture, row `I18`).

## CQ type follows the activation

RFC 0008 §4.4: adding the first reference (`act_refs` empty → non-empty) switches `cq_type` on its own — SOTA if any SOTA reference is present (a mixed activation defaults to SOTA), else POTA. Both boards' `porta_host_config_set()` detect exactly that one transition and set `cq_type` before applying the rest of the write, so the existing `sync_autoseq_from_globals()`/`sync_autoseq_station()` path (already run on every config write) picks it up with no extra plumbing. `act_refs_prefer_sota()` (`main/storage/station.cpp:429`) is the shared one-line predicate; the transition check itself stays separate per board, matching the shape their `porta_host_config_set()` functions already have. A later add or remove never re-derives, so it cannot clobber a manual switch made through the Station sheet's own Type control ([webapp.md](webapp.md), **The Station sheet**).

This also closes RFC 0008 §9's open question, "whether a TX already armed for the current slot picks up a CQ prefix switch": [autoseq.md](autoseq.md)'s head-check-before-the-trigger now refreshes an armed TX's text in place when the queue head's identity hasn't changed, so a switch made after a slot armed still lands in it, on both boards.
