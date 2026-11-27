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

// The UTC days that hold QSOs this station made, newest first, named as the daily files were
// (`20261125.adi`), which is how the QSOs tab still asks for them (FILE_LIST / FILE_READ).
void qso_log_store_days(std::vector<std::string>* names_out);

// A record as a page of the log hands it out: its ID, its QSO_DATE as the index holds it (20261125, 0 for
// none) and its fields.
struct QsoLogRecord {
    uint32_t id;
    uint32_t date;
    std::string fields;
};

// A day's QSOs in the order they were logged, after the first `skip`, at most `take`. `date` is 20261125.
// Returns how many that day holds in all.
size_t qso_log_store_day_records(uint32_t date, size_t skip, size_t take, std::vector<QsoLogRecord>* out);

// A page of records matching `filter`, newest first by date, time and ID, older than `before` (all zero
// for the newest), at most `take`. Returns how many match the filter in all.
size_t qso_log_store_page(const QsoStoreFilter& filter, const QsoStoreCursor& before, size_t take,
                          std::vector<QsoLogRecord>* out);

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
