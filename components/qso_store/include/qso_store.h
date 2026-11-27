#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The QSO store: every logged QSO as a record with an ID, in one append-only journal. No ESP-IDF and no
// file calls: the caller reads the journal into a buffer at start and hands over a function that appends
// to the file, so host tests and firmware link the same code.
//
// A record is its ADIF fields. Nothing is rewritten in place: a change to a record (QRZ's ID once it is
// uploaded, an edit) is a small journal entry naming the fields that changed, and a delete is one naming
// the record. The whole journal stays in RAM beside an index, so every query is answered from memory.
//
// The journal's format is what an update must keep; the index is rebuilt at every start and may change
// freely. Format, little-endian throughout:
//
//   file header, 24 bytes   "MFQS", u16 version (1), u16 header bytes (24), u64 store identity,
//                           u32 generation, u32 CRC-32 of the 20 bytes before it
//   entry header, 16 bytes  0xD5, u8 kind, u16 field bytes, u32 record ID, u32 previous entry for this
//                           record (a journal offset, 0 for none), u8 flags, three zero bytes
//   the fields              `<name:length>value` back to back, names lower case, no `<eor>`
//   u32 CRC-32              of the entry header without its previous-entry word, and the fields
//
// The previous-entry word is outside the CRC because loading rewrites it in RAM when the entry it named
// was lost to damage. An entry that fails its checks is skipped; when nothing readable follows it, it was
// a write cut short, and the journal ends before it.

inline constexpr size_t kQsoStoreHeaderBytes = 24;
inline constexpr size_t kQsoStoreEntryOverhead = 20;  // an entry's header and CRC
inline constexpr size_t kQsoStoreMaxFields = 1024;    // one entry's fields, in bytes
inline constexpr size_t kQsoStoreCallChars = 15;
// Distinct bands, and distinct modes, the index names. A record on one past that is kept and read back like
// any other; only qso_store_worked() asked for that band or mode does not see it.
inline constexpr size_t kQsoStoreNames = 48;
inline constexpr size_t kQsoStoreNameChars = 15;
inline constexpr size_t kQsoStoreMetaKeys = 8;
inline constexpr size_t kQsoStoreMetaKeyChars = 24;

// A record made elsewhere and brought in from the operator's online logbook: never uploaded, and no part
// of this station's own log.
inline constexpr uint8_t kQsoStoreForeign = 0x01;

inline constexpr uint8_t kQsoStoreNoName = 0xFF;

enum class QsoStoreStatus : uint8_t {
    OK,
    UNCHANGED,     // the record already says so: nothing was written
    FULL,          // no room in the journal's buffer, the index or the meta keys
    TOO_LONG,      // more than kQsoStoreMaxFields of fields
    BAD_FIELDS,    // not ADIF fields, or no usable CALL
    NOT_FOUND,
    WRITE_FAILED,  // the append function refused: the store is as it was
    BAD_HEADER,    // not a journal this build reads
    BAD_BUFFERS,   // see QsoStoreBuffers
};

inline bool qso_store_ok(QsoStoreStatus status)
{
    return status == QsoStoreStatus::OK || status == QsoStoreStatus::UNCHANGED;
}

// One record in the index. Rebuilt from the journal at load, so it can grow a member without a format
// change.
struct QsoStoreEntry {
    uint32_t id;
    uint32_t latest;     // journal offset of the record's newest entry; 0 once it is deleted
    uint64_t qrz_logid;  // APP_QRZLOG_LOGID, QRZ's ID for the record; 0 while it has none
    uint32_t date;       // QSO_DATE as a number (20261119); 0 when the record has none
    uint16_t time;       // TIME_ON's hours and minutes (2154)
    uint8_t band;        // index into the store's band names, kQsoStoreNoName for none
    uint8_t mode;        // index into its mode names: SUBMODE when the record has one, else MODE
    uint8_t flags;       // kQsoStoreForeign
    bool uploaded;       // QRZCOM_QSO_UPLOAD_STATUS is Y: QRZ holds it, though it may have told us no ID
    char call[kQsoStoreCallChars + 1];  // upper case
};

// The caller's memory, for the life of the store. On the P4 these are static, in PSRAM
// (docs/architecture/memory.md): the store never allocates them and never falls back to the heap.
struct QsoStoreBuffers {
    uint8_t* image;           // the journal, whole
    size_t image_bytes;
    QsoStoreEntry* entries;   // one per record ever added, deleted ones included
    size_t entry_count;
    uint32_t* call_slots;     // the call lookup table
    size_t call_slot_count;   // a power of two, at least twice entry_count
};

// Makes `len` bytes durable at the end of the journal's file. False means they were not saved.
using QsoStoreAppendFn = bool (*)(void* ctx, const uint8_t* bytes, size_t len);

struct QsoStoreNames {
    char name[kQsoStoreNames][kQsoStoreNameChars + 1];
    uint8_t used;
};

struct QsoStore {
    QsoStoreBuffers buffers;
    QsoStoreAppendFn append;
    void* append_ctx;
    uint64_t store_id;    // set when the journal is created: a reader's copy belongs to one identity
    uint32_t generation;  // and one generation of it
    size_t end;           // bytes of journal held; a reader that has read to here is caught up
    size_t entries_used;
    size_t slots_used;
    uint32_t next_id;
    QsoStoreNames bands;
    QsoStoreNames modes;
    uint32_t meta_at[kQsoStoreMetaKeys];  // journal offset of each key's newest value
    uint8_t meta_used;
};

struct QsoStoreLoad {
    QsoStoreStatus status;  // OK, BAD_HEADER, BAD_BUFFERS, or FULL when the index is too small for the journal
    size_t valid_bytes;     // where the journal ends: truncate the file to this when it is longer
    size_t damaged_bytes;   // bytes inside the journal that no entry could be read from
    uint32_t ignored;       // readable entries that named a record the journal no longer holds
};

// Starts an empty journal: writes its header through `append`.
QsoStoreStatus qso_store_create(QsoStore* store, const QsoStoreBuffers& buffers, uint64_t store_id,
                                QsoStoreAppendFn append, void* append_ctx);

// Takes up a journal the caller has read into buffers.image[0 .. bytes).
QsoStoreLoad qso_store_load(QsoStore* store, const QsoStoreBuffers& buffers, size_t bytes,
                            QsoStoreAppendFn append, void* append_ctx);

// A new record from ADIF fields (`<call:5>K1ABC <band:3>20M ...`, with or without `<eor>`). It needs a
// CALL of at most kQsoStoreCallChars. Fields of no length are left out. `flags` is 0 for a QSO this
// station made.
QsoStoreStatus qso_store_add(QsoStore* store, const char* fields, size_t len, uint8_t flags, uint32_t* id_out);

// Sets the named fields on a record, leaving its others; a field of no length (`<comment:0>`) removes it.
// Fields the record already holds with the same value are not written, and when none is left the answer is
// UNCHANGED.
QsoStoreStatus qso_store_patch(QsoStore* store, uint32_t id, const char* fields, size_t len);

QsoStoreStatus qso_store_delete(QsoStore* store, uint32_t id);

// A record as the operator's online logbook holds it, named by the APP_QRZLOG_LOGID among its fields. If a
// record here carries that ID, the fields are set on it as qso_store_patch() does, whoever made it, so the
// caller passes only the fields it means to take; otherwise it is added as foreign. Sent twice, the second
// is UNCHANGED.
QsoStoreStatus qso_store_import(QsoStore* store, const char* fields, size_t len, uint32_t* id_out);

// A record's fields as they now stand, and its index entry. False and nullptr for an ID that was never
// given out or is deleted.
bool qso_store_get(const QsoStore& store, uint32_t id, std::string* fields_out);
const QsoStoreEntry* qso_store_entry(const QsoStore& store, uint32_t id);

bool qso_store_find_qrz(const QsoStore& store, uint64_t qrz_logid, uint32_t* id_out);

// Worked before: the newest record with this call, own or foreign, on `band` and `mode` (either may be
// nullptr for any; mode is FT4 for a record logged MFSK with SUBMODE FT4), dated `since_date` or later
// (0 for ever). Names compare without regard to case. One table lookup, so it can be asked per decode.
//
// `my_refs` is nullptr or "" away from an activation. During one it is the station's references as its
// settings hold them ("POTA:US-1234,SOTA:W7O/CC-001"), and only a record logged at one of them counts:
// each park or summit is its own log, so a station worked from home that morning is not yet worked from
// the park, and a record logged at two parks counts at each. A reference with no program is a summit
// when it holds a `/`, else a park.
bool qso_store_worked(const QsoStore& store, const char* call, const char* band, const char* mode,
                      uint32_t since_date, const char* my_refs, uint32_t* id_out);

enum class QsoStoreWhose : uint8_t { ANY, OWN, FOREIGN };

struct QsoStoreFilter {
    uint32_t date = 0;                        // one QSO_DATE; 0 for every day
    QsoStoreWhose whose = QsoStoreWhose::ANY;
    bool unsent = false;                      // only records with no QRZ ID that are not marked uploaded
    bool newest_first = false;                // by ID: for own records, the order they were logged in
    const char* my_refs = nullptr;            // only records logged at one of these, as qso_store_worked() takes them
};

// The IDs of the records matching `filter`, after the first `skip` of them, at most `take`. Returns how
// many it wrote; `matched_out`, when given, is how many match in all. One pass over the index.
size_t qso_store_select(const QsoStore& store, const QsoStoreFilter& filter, size_t skip, uint32_t* ids,
                        size_t take, size_t* matched_out);

// Where a page of records newest first by QSO_DATE, TIME_ON and ID ends: the next page holds those older than
// it. All zero is the newest, before any page. A record with no date sorts oldest.
struct QsoStoreCursor {
    uint32_t date = 0;
    uint16_t time = 0;
    uint32_t id = 0;
};

inline bool qso_store_cursor_start(const QsoStoreCursor& c) { return c.date == 0 && c.time == 0 && c.id == 0; }

// The IDs of at most `take` records matching `filter` and older than `before`, newest first by date, time
// and ID (`filter.newest_first` is not read). Returns how many it wrote; `matched_out`, when given, is how
// many match the filter in all, whatever the cursor. One pass over the index, keeping the newest `take`.
size_t qso_store_page(const QsoStore& store, const QsoStoreFilter& filter, const QsoStoreCursor& before,
                      uint32_t* ids, size_t take, size_t* matched_out);

// A small value kept in the journal beside the records, such as how far a sync with the online logbook has
// got. Keys are lower-case letters, digits and `_`, at most kQsoStoreMetaKeyChars; the newest value wins.
QsoStoreStatus qso_store_meta_set(QsoStore* store, const char* key, const char* value);
bool qso_store_meta_get(const QsoStore& store, const char* key, std::string* value_out);

// Reading the journal in order, for a copy kept elsewhere (the browser's). Start at cursor 0, apply each
// change, and keep the cursor: later, only what was written since comes back. A copy is good for one
// store_id and generation; when either differs, it starts again from 0. A change that names a record the
// copy does not hold is passed over: damage to the journal can leave one.
enum class QsoStoreChangeKind : uint8_t {
    ADDED,       // a new record: its ID, flags and fields
    FIELDS_SET,  // fields set on a record, as qso_store_patch() takes them
    DELETED,
    META,        // fields is `key=value`
};

struct QsoStoreChange {
    QsoStoreChangeKind kind;
    uint8_t flags;
    uint32_t id;
    const char* fields;  // inside the store's journal: valid until the store is next loaded
    size_t len;
};

enum class QsoStoreFeed : uint8_t {
    CHANGE,      // `change_out` is filled and the cursor moved past it
    CAUGHT_UP,
    BAD_CURSOR,  // not a cursor this journal gave out: the copy starts again from 0
};

QsoStoreFeed qso_store_feed(const QsoStore& store, size_t* cursor_io, QsoStoreChange* change_out);
