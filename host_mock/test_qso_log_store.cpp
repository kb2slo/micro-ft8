// qso_log_store: the station's QSO log on the QSO store. Boot (load, convert the daily files, start empty),
// what survives a cut, the day-shaped reads the QSOs tab still asks for, and deletes. Storage is stood in for
// here by a map of file names to bytes, so every start reads back exactly what the last one wrote.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "qso_browse.h"
#include "qso_log_store.h"
#include "storage_service.h"

static int g_fails = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
            ++g_fails;                                                \
        }                                                             \
    } while (0)

// --- storage, in memory ---------------------------------------------------------------------------------
static std::map<std::string, std::string> g_files;
static bool g_storage_up = true;
static bool g_fail_appends = false;
static bool g_fail_removes = false;

bool storage_service_firmware_available() { return g_storage_up; }
bool storage_file_exists(const std::string& name) { return g_storage_up && g_files.count(name) != 0; }
bool storage_file_remove(const std::string& name)
{
    if (g_fail_removes) return false;
    return g_files.erase(name) != 0;
}
bool storage_file_list(std::vector<std::string>& files)
{
    files.clear();
    for (const auto& kv : g_files) files.push_back(kv.first);
    return g_storage_up;
}
bool storage_file_read_text(const std::string& name, std::string& content)
{
    auto it = g_files.find(name);
    if (it == g_files.end()) return false;
    content = it->second;
    return true;
}
bool storage_file_write_atomic(const std::string& name, const std::string& content)
{
    g_files[name] = content;
    return true;
}
bool storage_file_size(const std::string& name, size_t* size_out)
{
    auto it = g_files.find(name);
    if (it == g_files.end()) return false;
    *size_out = it->second.size();
    return true;
}
bool storage_file_read_into(const std::string& name, uint8_t* buffer, size_t capacity, size_t* len_out)
{
    auto it = g_files.find(name);
    if (it == g_files.end() || it->second.size() > capacity) return false;
    memcpy(buffer, it->second.data(), it->second.size());
    *len_out = it->second.size();
    return true;
}
bool storage_file_truncate(const std::string& name, size_t size)
{
    auto it = g_files.find(name);
    if (it == g_files.end() || size > it->second.size()) return false;
    it->second.resize(size);
    return true;
}
bool storage_file_append(const std::string& name, const std::string& content, const std::string& header_if_new, bool)
{
    if (g_fail_appends) return false;  // all or nothing, as the real one is
    std::string& f = g_files[name];
    if (f.empty()) f = header_if_new;
    f += content;
    return true;
}

// --- helpers ----------------------------------------------------------------------------------------------
static const char kHeader[] = "ADIF EXPORT\n<eoh>\n";

// A record as adif_format_log_record() writes it.
static std::string record(const char* call, const char* date, const char* time_on, const char* band = "20M")
{
    char buf[256];
    snprintf(buf, sizeof buf,
             "<call:%u>%s <gridsquare:4>FN42 <mode:3>FT8<qso_date:8>%s <time_on:6>%s <freq:6>14.074 "
             "<band:%u>%s <station_callsign:6>N0CALL <my_gridsquare:4>EM10 <rst_sent:3>-10 <rst_rcvd:3>-12 "
             "<comment:3>QMX <eor>\n",
             (unsigned)strlen(call), call, date, time_on, (unsigned)strlen(band), band);
    return buf;
}

static std::vector<std::string> days()
{
    std::vector<std::string> d;
    qso_log_store_days(&d);
    return d;
}

static std::string call_of(const std::string& fields)
{
    const size_t at = fields.find("<call:");
    if (at == std::string::npos) return "";
    const size_t gt = fields.find('>', at);
    const size_t len = (size_t)atoi(fields.c_str() + at + 6);
    return fields.substr(gt + 1, len);
}

static void reset() { g_files.clear(); g_storage_up = true; g_fail_appends = false; g_fail_removes = false; }

// --- tests ------------------------------------------------------------------------------------------------
static void test_start_empty()
{
    reset();
    CHECK(qso_log_store_start() && qso_log_store_ready(), "starts with no journal and no daily files");
    CHECK(g_files.count(kQsoLogJournal) && g_files[kQsoLogJournal].size() == kQsoStoreHeaderBytes,
          "an empty journal is written: %u bytes", (unsigned)g_files[kQsoLogJournal].size());
    CHECK(days().empty(), "and holds no day");
    g_storage_up = false;
    CHECK(!qso_log_store_start() && !qso_log_store_ready() && qso_log_store() == nullptr, "no storage, no log");
    CHECK(qso_log_store_add(record("K1ABC", "20261125", "120000"), nullptr) == QsoStoreStatus::WRITE_FAILED,
          "and a QSO is refused, not lost silently");
}

static void test_convert()
{
    reset();
    g_files["20261124.adi"] = kHeader + record("K1ABC", "20261124", "101500") + record("W9XY", "20261124", "102000");
    g_files["20261125.adi"] = kHeader + record("DL1AA", "20261125", "090000", "40M");
    g_files["Station.txt"] = "call=N0CALL\n";
    CHECK(qso_log_store_start(), "converts the daily files");
    CHECK(!g_files.count("20261124.adi") && !g_files.count("20261125.adi"), "and deletes them, every record taken");
    CHECK(g_files.count("Station.txt"), "leaving the settings alone");
    const std::vector<std::string> d = days();
    CHECK(d.size() == 2 && d[0] == "20261125.adi" && d[1] == "20261124.adi", "days newest first, as the files were");
    std::vector<std::string> recs;
    CHECK(qso_log_store_day_records(20261124, 0, 10, &recs) == 2 && recs.size() == 2, "a day's QSOs");
    CHECK(recs.size() == 2 && call_of(recs[0]) == "K1ABC" && call_of(recs[1]) == "W9XY", "in the order logged");
    const QsoStore* store = qso_log_store();
    uint32_t id = 0;
    CHECK(qso_store_worked(*store, "DL1AA", "40M", "FT8", 0, nullptr, &id) && id == 3,
          "IDs follow the days oldest first: %u", (unsigned)id);

    // What the QSOs tab reads is the same row the daily file gave.
    QsoBrowsePager from_file;
    QsoBrowsePager from_store;
    qso_browse_pager_reset(&from_file, 0, 10);
    qso_browse_pager_reset(&from_store, 0, 10);
    qso_browse_pager_feed(&from_file, record("K1ABC", "20261124", "101500"), nullptr, 0);
    qso_browse_pager_feed(&from_store, recs[0] + "<eor>", nullptr, 0);
    const QsoLogEntry& a = from_file.entries.at(0);
    const QsoLogEntry& b = from_store.entries.at(0);
    CHECK(a.call == b.call && a.time_on == b.time_on && a.band == b.band && a.grid == b.grid && a.freq == b.freq &&
              a.my_grid == b.my_grid && a.comment == b.comment && a.mode == b.mode &&
              a.station_callsign == b.station_callsign && a.rst_sent == b.rst_sent && a.rst_rcvd == b.rst_rcvd,
          "a record from the store makes the row its daily-file line made");

    // Restarted, the journal is loaded and nothing is converted again.
    const std::string journal = g_files[kQsoLogJournal];
    CHECK(qso_log_store_start() && days().size() == 2 && g_files[kQsoLogJournal] == journal,
          "a second start loads what the first wrote");
}

static void test_convert_refused()
{
    reset();
    g_files["20261124.adi"] = kHeader + record("K1ABC", "20261124", "101500") + "<band:3>20M <mode:3>FT8 <eor>\n";
    CHECK(qso_log_store_start(), "a record with no call does not stop the start");
    CHECK(g_files.count("20261124.adi"), "the daily files are kept when a record was not taken");
    CHECK(days().size() == 1, "the records that were taken are in the store");
    CHECK(qso_log_store_start() && g_files.count("20261124.adi") && days().size() == 1,
          "and a restart keeps them, converting nothing twice");
}

static void test_cut_between_write_and_delete()
{
    reset();
    g_files["20261124.adi"] = kHeader + record("K1ABC", "20261124", "101500");
    g_fail_removes = true;  // as if the power went after the journal was written
    CHECK(qso_log_store_start() && g_files.count("20261124.adi"), "the journal is written before the files go");
    g_fail_removes = false;
    CHECK(qso_log_store_start() && !g_files.count("20261124.adi"), "the next start deletes the files it converted");
    CHECK(days().size() == 1, "with the QSO there once: %u days", (unsigned)days().size());
    std::vector<std::string> recs;
    CHECK(qso_log_store_day_records(20261124, 0, 10, &recs) == 1, "once");
}

static void test_log_and_restart()
{
    reset();
    CHECK(qso_log_store_start(), "start");
    uint32_t id = 0;
    CHECK(qso_log_store_add(record("K1ABC", "20261125", "120000"), &id) == QsoStoreStatus::OK && id == 1, "logs a QSO");
    const size_t size = g_files[kQsoLogJournal].size();
    CHECK(size > kQsoStoreHeaderBytes, "into the journal's file");
    CHECK(qso_log_store_start() && days().size() == 1, "kept across a restart");

    // A write cut short leaves bytes past the last whole entry: the next start cuts them off.
    g_files[kQsoLogJournal] += std::string("\xD5\x01\x20\x00junk", 8);
    CHECK(qso_log_store_start() && g_files[kQsoLogJournal].size() == size, "a torn tail is cut off at start");
    CHECK(qso_log_store_add(record("W9XY", "20261125", "121000"), &id) == QsoStoreStatus::OK, "and logging goes on");
    CHECK(qso_log_store_start(), "start");
    std::vector<std::string> recs;
    CHECK(qso_log_store_day_records(20261125, 0, 10, &recs) == 2, "with both QSOs kept: %u", (unsigned)recs.size());

    // A failed append leaves the file and the store as they were.
    const std::string before = g_files[kQsoLogJournal];
    g_fail_appends = true;
    CHECK(qso_log_store_add(record("DL1AA", "20261125", "122000"), &id) == QsoStoreStatus::WRITE_FAILED,
          "a failed write is reported");
    g_fail_appends = false;
    CHECK(g_files[kQsoLogJournal] == before && qso_log_store_day_records(20261125, 0, 10, &recs) == 2,
          "and nothing of it stays");
    CHECK(qso_log_store_add(record("DL1AA", "20261125", "122000"), &id) == QsoStoreStatus::OK &&
              qso_log_store_start() && qso_log_store_day_records(20261125, 0, 10, &recs) == 3,
          "the next write lands, and loads");
    g_files["qso.tmp"] = "half a journal";
    CHECK(qso_log_store_start() && !g_files.count("qso.tmp"), "a temporary journal left by a cut is removed");
}

static void test_pages_and_deletes()
{
    reset();
    CHECK(qso_log_store_start(), "start");
    for (int i = 0; i < 7; ++i) {
        char call[8];
        char t[8];
        snprintf(call, sizeof call, "K%dABC", i);
        snprintf(t, sizeof t, "1%05d", i);
        qso_log_store_add(record(call, "20261125", t), nullptr);
    }
    qso_log_store_add(record("W9XY", "20261124", "100000"), nullptr);
    std::vector<std::string> recs;
    CHECK(qso_log_store_day_records(20261125, 5, 4, &recs) == 7 && recs.size() == 2 && call_of(recs[0]) == "K5ABC",
          "a page: skip and take, and how many in all");
    CHECK(qso_log_store_delete_day(20261124) == nullptr && days().size() == 1 && days()[0] == "20261125.adi",
          "a day's QSOs deleted");
    CHECK(qso_log_store_delete_day(20261124) != nullptr, "a day with none says so");
    CHECK(qso_log_store_start() && days().size() == 1, "and the delete is kept");
    g_files["20261101.adi"] = kHeader + record("K1ABC", "20261101", "101500");  // left from a partial conversion
    const uint64_t old_id = qso_log_store()->store_id;
    CHECK(qso_log_store_clear() == nullptr && days().empty(), "clear deletes every QSO");
    CHECK(!g_files.count("20261101.adi"), "and any daily file left");
    CHECK(qso_log_store()->store_id != old_id, "a new journal, so a reader's copy starts again");
    CHECK(qso_log_store_start() && days().empty() && g_files[kQsoLogJournal].size() == kQsoStoreHeaderBytes,
          "and stays cleared");
}

static void test_unreadable_journal()
{
    reset();
    g_files[kQsoLogJournal] = "not a journal at all, and longer than a header";
    const std::string kept = g_files[kQsoLogJournal];
    CHECK(!qso_log_store_start() && !qso_log_store_ready(), "a journal this build cannot read: not ready");
    CHECK(g_files[kQsoLogJournal] == kept, "and it is left as it is, not written over");
    std::vector<std::string> d;
    qso_log_store_days(&d);
    CHECK(d.empty() && qso_log_store_delete_day(20261125) != nullptr, "nothing is answered from it");
}

static void test_day_of()
{
    CHECK(qso_log_day_of("20261125.adi") == 20261125, "a daily name");
    CHECK(qso_log_day_of("20261125.txt") == 0 && qso_log_day_of("2026112.adi") == 0 &&
              qso_log_day_of("2026112x.adi") == 0 && qso_log_day_of("Station.txt") == 0 && qso_log_day_of(nullptr) == 0,
          "anything else is no day");
}

int main()
{
    test_start_empty();
    test_convert();
    test_convert_refused();
    test_cut_between_write_and_delete();
    test_log_and_restart();
    test_pages_and_deletes();
    test_unreadable_journal();
    test_day_of();
    if (g_fails) {
        fprintf(stderr, "%d FAIL(s)\n", g_fails);
        return 1;
    }
    printf("PASS: qso log store\n");
    return 0;
}
