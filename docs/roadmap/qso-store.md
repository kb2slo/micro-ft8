# QSO store (epic I32)

Every logged QSO a record with an ID on the main board, answered where the log lives instead of read a day file at a time into the browser. The store is the main board's log ([logging.md](../architecture/logging.md), **The store on the main board**; its format and calls in `components/qso_store/include/qso_store.h`). Rows: [qtfj](#qtfj) ([ROADMAP.md](../ROADMAP.md)).

## Goal

**Every QSO is a record with an ID on the main board; the operator's QRZ logbook and the station agree; the log outlives any one phone.**

## Constraints

* **From 1.0 an update keeps the log** ([release-1-0.md](release-1-0.md#n75z)): the journal's format is the one an update must keep; the index is rebuilt at every start and may change freely.
* **Memory:** the journal and its index are held whole in RAM, in static buffers in PSRAM that the caller supplies; the store never allocates them ([memory.md](../architecture/memory.md)).
* **Nothing is sized to a Port A frame.** Where a record does not fit one, the answer is a stream ([syb2](ideas.md#syb2)), not a narrower record.

## Decided

* **This journal, not SQLite.** One append-only file: a record is its ADIF fields under a 32-bit ID; a change is a small entry naming only the fields that changed; a delete is one entry; nothing is rewritten in place. Each entry carries a CRC, so a write cut short or a damaged entry is dropped at load and logging carries on.
* **A record is named fields from end to end.** Code names the fields it needs and passes the rest through; no wire carries a record as fixed positions.
* **Our record ID is the identity.** A service's ID (QRZ's `APP_QRZLOG_LOGID`) is one reference among several, since LoTW, POTA and SOTA issue none. A record counts as sent to QRZ when it carries QRZ's ID or `QRZCOM_QSO_UPLOAD_STATUS` Y: QRZ refuses a QSO it already holds without naming its ID.
* **A sync service is** which records it takes, how each is written for it, how they match, and a transport. The QRZ sync built before the store ([webapp.md](../architecture/webapp.md), **QRZ sync**) matches against QRZ on every run and keeps nothing on the station; it gains the store's sent mark in [qtfj](#qtfj).
* **Worked before counts only at the references it was worked from:** during an activation each park or summit is its own log, and an n-fer record counts at each.
* **Awards are decode cues only.**
* **The daily files are converted once, at the main board's first boot on the store** (by exception, maintainer, 2026-11-19, against **Before 1.0: do not carry the past**). When the conversion comes out is open.

## Children

In order. Only the next is a row in the index.

* <a id="qtfj"></a>**qtfj: set fields on a record, and the record's ID in the browser.** One generic "set fields on record N" message, and each row's ID carried to the web app; the QRZ sync then writes QRZ's ID and the confirmed flag back after a match, and reads the store's unsent records instead of re-matching every day since the oldest unsent. **Done-when:** host tests hold the message and the sync's write-back; a QSO sent to QRZ reads as sent on a second phone with no QRZ call (field). **Lands in:** [logging.md](../architecture/logging.md), [webapp.md](../architecture/webapp.md) (**QRZ sync**), [sidekick.md](../architecture/sidekick.md) (Port A).
* <a id="r4cu"></a>**r4cu: import the operator's QRZ logbook.** The web app fetches it through its QRZ relay and sends each record, cut to the fields the station reads, to the main board's `qso_store_import()`: a record with a QRZ ID the store holds is updated, any other added as foreign. Kept: our logged fields, QRZ's ID and confirmation (`app_qrzlog_status`, `app_qrzlog_qsldate`), `dxcc`, `cont`, `state`, `cnty`, `cqz`, `ituz`, and a `gridsquare` ours lacks; not QRZ's copy of our station, nor the other operator's name, address or email. Measured 2026-11-25: QRZ held the board's 294 QSOs and 1,218 earlier ones no longer on it; 904 bytes a record, 340 cut, past one Port A frame. After [qtfj](#qtfj), so our own records match by ID instead of being added twice. **Open:** the transfer ([syb2](ideas.md#syb2), or chunks as a firmware image is sent); `name` for the card; foreign records in the QSOs list. **Done-when:** host tests hold the kept fields and an own record updated, not added; at home the maintainer's 1,512 records import with none refused, a station worked only before 2026-11-11 shows those QSOs on its card, and boot load is measured after (field). **Lands in:** [logging.md](../architecture/logging.md), [webapp.md](../architecture/webapp.md) (**QRZ sync**), [sidekick.md](../architecture/sidekick.md) (Port A); the site (its control); TEST_PLAN Pending.
* <a id="w4kn"></a>**w4kn: the main board marks worked decodes, over the whole log.** Issue #36. Operate's **Worked** flag reads today's log every 8 s ([webapp.md](../architecture/webapp.md), **Worked and activation cues on decodes**), each read making every other request to the main board wait ([sidekick.md](../architecture/sidekick.md), **Tasks and cores**). The main board answers instead, from `qso_store_worked()`, and puts a worked bit on each DECODE event; the app's log read for it goes. Port A protocol change. **Later:** the progress line's and the QSOs tab's polls; the **Hide worked** filter then reads the bit. **Done-when:** host tests cover band, mode, references and the 00:00 UTC rollover; at home a station worked on an earlier day is flagged in its next slot with no log read (field). **Lands in:** [webapp.md](../architecture/webapp.md) (**Worked and activation cues on decodes**), [sidekick.md](../architecture/sidekick.md) (Port A); the site if the cue's wording changes.

**Later:**

* The callsign card's QSO history from the whole log: a page of records by call, which the store's queries do not yet offer.
* The QSOs list read in pages from the store instead of day files.
* A stream over Port A ([syb2](ideas.md#syb2)), and the fixed 255-byte QSO row deleted.
* Entity and state on our records.
* "Needed" cues from the imported entity, state and confirmation ([smart-queue.md](smart-queue.md)).
* The browser's copy and the map's layers read from the store.
* A reference set after logging ([w5nj](../ROADMAP.md#w5nj)).
* Edit and delete ([I31](ideas.md#I31)).
* A per-park upload mark for POTA.
* Compaction of the journal.

## Open

* **`storage` grown before 1.0** ([n75z](release-1-0.md#n75z)): a record is 209 to 235 bytes as the station writes it, QRZ's 904, 340 cut to [r4cu](#r4cu)'s fields (`test_apps/qso_store_bench`, 2026-11-25).
* **A record missing a field** is marked incomplete rather than refused (for [r4cu](#r4cu)); today only one with no call is refused.
* **When the conversion of daily files comes out** (`convert_or_create()`, `qso_log_store.cpp`).
* **An ID reused after damage:** an ID is never reused in normal running, but if damage destroys the newest record its ID is given out again (`qso_store.cpp`); whether that matters once [qtfj](#qtfj) puts IDs in the browser.

## Why not

* **SQLite:** its amalgamation compiled for the P4 at the build's `-Og` adds 546 KB to an 870 KB image, and its durability here would rest on a LittleFS shim only a pulled plug on hardware could prove (PR #109, 2026-11-19).
