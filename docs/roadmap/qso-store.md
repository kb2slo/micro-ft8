# QSO store (epic I32)

Every logged QSO a record with an ID on the main board, answered where the log lives instead of read a day file at a time into the browser. The store is the main board's log ([logging.md](../architecture/logging.md), **The store on the main board**; its format and calls in `components/qso_store/include/qso_store.h`). Rows: [w4kn](#w4kn) ([ROADMAP.md](../ROADMAP.md)).

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
* **A sync service is** which records it takes, how each is written for it, how they match, and a transport. The QRZ sync ([webapp.md](../architecture/webapp.md), **QRZ sync**) reads the records the store has not marked sent and writes back what QRZ says of them.
* **Worked before counts only at the references it was worked from:** during an activation each park or summit is its own log, and an n-fer record counts at each.
* **Awards are decode cues only.**
* **The daily files are converted once, at the main board's first boot on the store** (by exception, maintainer, 2026-11-19, against **Before 1.0: do not carry the past**), **and deleted once every record in them is taken** (maintainer, 2026-11-26): not kept as a fallback. One refused keeps them all. When the conversion comes out is open.

## Children

In order. Only the next is a row in the index.

* <a id="w4kn"></a>**w4kn: the main board marks worked decodes, over the whole log.** Issue #36. Operate's **Worked** flag reads today's log every 8 s ([webapp.md](../architecture/webapp.md), **Worked and activation cues on decodes**), each read making every other request to the main board wait ([sidekick.md](../architecture/sidekick.md), **Tasks and cores**). The main board answers instead, from `qso_store_worked()`, and puts a worked bit on each DECODE event; the app's log read for it goes. Port A protocol change. **Later:** the progress line's and the QSOs tab's polls; the **Hide worked** filter then reads the bit. **Done-when:** host tests cover band, mode, references and the 00:00 UTC rollover; at home a station worked on an earlier day is flagged in its next slot with no log read (field). **Lands in:** [webapp.md](../architecture/webapp.md) (**Worked and activation cues on decodes**), [sidekick.md](../architecture/sidekick.md) (Port A); the site if the cue's wording changes.

**Later:**

* A stream over Port A ([syb2](ideas.md#syb2)), and the fixed 255-byte QSO row deleted.
* Entity and state on our records.
* "Needed" cues from the imported entity, state and confirmation ([smart-queue.md](smart-queue.md)).
* The browser's copy and the map's layers read from the store.
* A reference set after logging ([w5nj](../ROADMAP.md#w5nj)).
* Edit and delete ([I31](ideas.md#I31)).
* A per-park upload mark for POTA.
* Compaction of the journal.

## Open

* **`storage` grown before 1.0** ([n75z](release-1-0.md#n75z)): a record is 209 to 235 bytes as the station writes it, QRZ's 904, 340 cut to the import's fields (`test_apps/qso_store_bench`, 2026-11-25).
* **When the conversion of daily files comes out** (`convert_or_create()`, `qso_log_store.cpp`).
* **An ID reused after damage:** an ID is never reused in normal running, but if damage destroys the newest record its ID is given out again (`qso_store.cpp`); whether that matters now that the browser holds IDs (the QRZ sync's write-back names a record by one).

## Why not

* **SQLite:** its amalgamation compiled for the P4 at the build's `-Og` adds 546 KB to an 870 KB image, and its durability here would rest on a LittleFS shim only a pulled plug on hardware could prove (PR #109, 2026-11-19).
