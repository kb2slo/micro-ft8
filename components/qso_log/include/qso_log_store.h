#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "qso_store.h"

// The station's QSO log: the QSO store (components/qso_store) in one journal file on `storage`, held whole in
// PSRAM (docs/architecture/logging.md, The store on the main board). Called from the main loop only, which
// logs QSOs and answers Port A, and at boot before it starts; nothing here is safe from another task.

// The journal's file on `storage`.
inline constexpr char kQsoLogJournal[] = "qso.jrn";

// At boot, once storage is up: loads the journal, or, with none, converts the daily `.adi` files into one
// (deleting them only when every record in them was taken), or starts an empty one. False when the station
// cannot log: storage is not up, or the journal cannot be read or written. Logs what it did and how long.
bool qso_log_store_start();
bool qso_log_store_ready();

// A QSO as ADIF fields (`adif_format_log_record()`'s line).
QsoStoreStatus qso_log_store_add(const std::string& record, uint32_t* id_out);

// The store, for reads; nullptr when not ready.
const QsoStore* qso_log_store();

// A record as a page of the log hands it out: its ID, its QSO_DATE as the index holds it (20261125, 0 for
// none) and its fields.
struct QsoLogRecord {
    uint32_t id;
    uint32_t date;
    bool imported;  // from the operator's QRZ logbook (kQsoStoreForeign), not logged by this station
    std::string fields;
};

// A page of records matching `filter`, newest first by date, time and ID, older than `before` (all zero
// for the newest), at most `take`. Returns how many match the filter in all.
size_t qso_log_store_page(const QsoStoreFilter& filter, const QsoStoreCursor& before, size_t take,
                          std::vector<QsoLogRecord>* out, QsoStoreSummary* summary_out = nullptr);

// A band or mode index's name, as the store holds it ("20M", "FT8").
const char* qso_log_store_band_name(uint8_t band);
const char* qso_log_store_mode_name(uint8_t mode);

// What the log says of a station heard now (docs/architecture/webapp.md, Worked and activation cues on
// decodes), as bits: a dupe (worked today on this band, at one of `refs` when the station has any), worked on
// this band any day, worked on any band. FT8 and FT4 count as one mode; imported QSOs count too. `today` is
// the UTC date (20261127), 0 while the clock is unset, which leaves out the dupe; `refs` as the station's
// settings hold them (qso_store_worked()). 0 for no call, or while the log is not ready.
inline constexpr uint8_t kQsoWorkedDupe = 0x01;
inline constexpr uint8_t kQsoWorkedBand = 0x02;
inline constexpr uint8_t kQsoWorkedEver = 0x04;
uint8_t qso_log_store_worked(const char* call, const char* band, uint32_t today, const char* refs);

// A record, or a slice of one, from the operator's QRZ logbook, named by its QRZ ID: added as imported, or set
// on the record that carries that ID. A QSO this station logged takes only QRZ's ID and confirmation, the
// region fields and a grid it lacks (kQsoLogImportOwnTakes); what the station logged stays as it logged it.
inline constexpr char kQsoLogImportOwnTakes[] =
    "app_qrzlog_logid,app_qrzlog_status,app_qrzlog_qsldate,dxcc,cont,state,cnty,cqz,ituz";
QsoStoreStatus qso_log_store_import(const std::string& fields);

// Sets fields on a record, as qso_store_patch() takes them: any field, one of no length removing it.
QsoStoreStatus qso_log_store_set(uint32_t id, const std::string& fields);

// Deletes a day's QSOs. Returns nullptr, or why not.
const char* qso_log_store_delete_day(uint32_t date);

// Deletes every QSO: a new, empty journal in place of the old, and any daily file left from before.
const char* qso_log_store_clear();

// The day a daily-file name names (`20261125.adi` gives 20261125); 0 for any other name.
uint32_t qso_log_day_of(const char* name);

// One daily file's text into the store: each record after the header, ended by `<eor>`. Counts the records
// taken and those refused (reasons are logged). Exposed for host tests.
void qso_log_convert_text(QsoStore* store, const std::string& text, const char* file, size_t* added, size_t* refused);
