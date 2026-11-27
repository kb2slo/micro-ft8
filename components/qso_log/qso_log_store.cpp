// The station's QSO log in the QSO store (qso_log_store.h, docs/architecture/logging.md).

#include "qso_log_store.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "qso_browse.h"
#include "storage_service.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_random.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

namespace {

const char* TAG = "qso_log_store";

// Sized to `storage` (3 MB, poe_p4/partitions.csv), so the journal outgrows the flash before the buffer:
// what the QSOs tab's free-space line counts is the limit (docs/architecture/memory.md). At the 209 to 235
// bytes a record the station writes (test_apps/qso_store_bench), about 13,000 QSOs; the index has room for
// more entries than that, deleted ones included.
constexpr size_t kImageBytes = 3u << 20;
constexpr size_t kEntries = 16384;
constexpr size_t kCallSlots = 32768;

// Static in PSRAM, with this file their one owner (docs/architecture/memory.md, Adding a buffer).
EXT_RAM_BSS_ATTR uint8_t s_image[kImageBytes];
EXT_RAM_BSS_ATTR QsoStoreEntry s_entries[kEntries];
EXT_RAM_BSS_ATTR uint32_t s_call_slots[kCallSlots];

QsoStore s_store;
bool s_ready = false;

// Whether the daily files were all converted, kept in the journal so a start after a cut between writing it
// and deleting them still deletes them.
constexpr char kConvertedKey[] = "converted";

QsoStoreBuffers buffers() { return QsoStoreBuffers{s_image, kImageBytes, s_entries, kEntries, s_call_slots, kCallSlots}; }

uint64_t new_store_id()
{
#ifdef ESP_PLATFORM
    return ((uint64_t)esp_random() << 32) | esp_random();
#else
    static uint64_t n = 0x5EED0000;
    return ++n;
#endif
}

bool append_journal(void*, const uint8_t* bytes, size_t len)
{
    return storage_file_append(kQsoLogJournal, std::string((const char*)bytes, len), "", true);
}

// For a journal built in RAM and written whole afterwards: the image already holds the bytes.
bool append_in_ram(void*, const uint8_t*, size_t) { return true; }

std::string journal_bytes() { return std::string((const char*)s_image, s_store.end); }

void daily_files(std::vector<std::string>* out)
{
    out->clear();
    std::vector<std::string> all;
    std::vector<std::string> daily;
    if (!storage_file_list(all)) return;
    qso_browse_select_daily_files(all, &daily);  // newest first
    for (const std::string& name : daily) {
        if (qso_log_day_of(name.c_str()) != 0) out->push_back(name);
    }
    std::reverse(out->begin(), out->end());  // converted oldest first, so IDs follow the order QSOs were logged
}

void remove_daily_files()
{
    std::vector<std::string> files;
    daily_files(&files);
    size_t removed = 0;
    for (const std::string& name : files) {
        if (storage_file_remove(name)) {
            ++removed;
        } else {
            ESP_LOGW(TAG, "could not delete %s", name.c_str());
        }
    }
    if (removed != 0) ESP_LOGI(TAG, "deleted %u daily files", (unsigned)removed);
}

bool load_journal(size_t size)
{
    size_t got = 0;
    if (!storage_file_read_into(kQsoLogJournal, s_image, kImageBytes, &got)) {
        ESP_LOGE(TAG, "%s: %u bytes, could not be read into its %u-byte buffer", kQsoLogJournal, (unsigned)size,
                 (unsigned)kImageBytes);
        return false;
    }
    const QsoStoreLoad load = qso_store_load(&s_store, buffers(), got, append_journal, nullptr);
    if (load.status != QsoStoreStatus::OK) {
        ESP_LOGE(TAG, "%s not loaded: status %d; the station will not log until it is", kQsoLogJournal,
                 (int)load.status);
        return false;
    }
    if (load.valid_bytes < got) {
        // A write cut short: the journal ends before it, and the next entry goes where it began.
        const bool cut = storage_file_truncate(kQsoLogJournal, load.valid_bytes);
        ESP_LOGW(TAG, "%s: %u bytes after the last whole entry, cut %s", kQsoLogJournal,
                 (unsigned)(got - load.valid_bytes), cut ? "off" : "FAILED");
        if (!cut) return false;
    }
    if (load.damaged_bytes != 0 || load.ignored != 0) {
        ESP_LOGW(TAG, "%s: %u damaged bytes passed over, %u entries naming no record", kQsoLogJournal,
                 (unsigned)load.damaged_bytes, (unsigned)load.ignored);
    }
    std::string converted;
    if (qso_store_meta_get(s_store, kConvertedKey, &converted) && converted == "all") remove_daily_files();
    return true;
}

// The daily files into a journal built in RAM, written whole once, then the files deleted if every record
// was taken. A cut before the write leaves the files and no journal, so the next start converts again.
bool convert_or_create()
{
    if (qso_store_create(&s_store, buffers(), new_store_id(), append_in_ram, nullptr) != QsoStoreStatus::OK) {
        return false;
    }
    std::vector<std::string> files;
    daily_files(&files);
    size_t added = 0;
    size_t refused = 0;
    for (const std::string& name : files) {
        std::string text;
        if (!storage_file_read_text(name, text)) {
            ESP_LOGE(TAG, "%s could not be read: every daily file is kept", name.c_str());
            ++refused;
            continue;
        }
        qso_log_convert_text(&s_store, text, name.c_str(), &added, &refused);
    }
    if (!files.empty()) qso_store_meta_set(&s_store, kConvertedKey, refused == 0 ? "all" : "partial");
    if (!storage_file_write_atomic(kQsoLogJournal, journal_bytes())) {
        ESP_LOGE(TAG, "%s could not be written; the station will not log until it is", kQsoLogJournal);
        return false;
    }
    s_store.append = append_journal;
    if (files.empty()) {
        ESP_LOGI(TAG, "%s started empty", kQsoLogJournal);
    } else {
        ESP_LOGI(TAG, "%u daily files converted: %u QSOs taken, %u refused", (unsigned)files.size(),
                 (unsigned)added, (unsigned)refused);
        if (refused == 0) {
            remove_daily_files();
        } else {
            ESP_LOGW(TAG, "daily files kept: %u records were not taken", (unsigned)refused);
        }
    }
    return true;
}

}  // namespace

uint32_t qso_log_day_of(const char* name)
{
    if (name == nullptr || strlen(name) != 12 || strcmp(name + 8, ".adi") != 0) return 0;
    uint32_t date = 0;
    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') return 0;
        date = date * 10 + (uint32_t)(name[i] - '0');
    }
    return date;
}

void qso_log_convert_text(QsoStore* store, const std::string& text, const char* file, size_t* added, size_t* refused)
{
    auto find_tag = [&](const char* tag, size_t from) {
        const size_t len = strlen(tag);
        for (size_t i = from; i + len <= text.size(); ++i) {
            size_t k = 0;
            while (k < len && (char)(text[i + k] | 0x20) == tag[k]) ++k;
            if (k == len) return i;
        }
        return std::string::npos;
    };
    const size_t eoh = find_tag("<eoh>", 0);
    size_t at = eoh == std::string::npos ? 0 : eoh + 5;
    size_t index = 0;
    for (;;) {
        const size_t eor = find_tag("<eor>", at);
        if (eor == std::string::npos) break;
        ++index;
        uint32_t id = 0;
        const QsoStoreStatus st = qso_store_add(store, text.data() + at, eor - at, 0, &id);
        if (st == QsoStoreStatus::OK) {
            ++*added;
        } else {
            ++*refused;
            ESP_LOGW(TAG, "%s record %u not taken: status %d", file, (unsigned)index, (int)st);
        }
        at = eor + 5;
    }
}

bool qso_log_store_start()
{
    s_ready = false;
    if (!storage_service_firmware_available()) {
        ESP_LOGE(TAG, "storage is not up: the station will not log");
        return false;
    }
    const int64_t start_us = esp_timer_get_time();
    // A write of the journal cut short leaves its temporary file; the journal itself is untouched.
    if (storage_file_exists("qso.tmp")) storage_file_remove("qso.tmp");
    size_t size = 0;
    const bool existed = storage_file_size(kQsoLogJournal, &size);
    s_ready = existed ? load_journal(size) : convert_or_create();
    size_t qsos = 0;
    for (size_t i = 0; s_ready && i < s_store.entries_used; ++i) qsos += s_entries[i].latest != 0 ? 1 : 0;
    ESP_LOGI(TAG, "%s %s: %u QSOs, %u bytes, in %lld ms", kQsoLogJournal,
             !s_ready ? "NOT READY" : existed ? "loaded" : "made", (unsigned)qsos, (unsigned)(s_ready ? s_store.end : 0),
             (long long)((esp_timer_get_time() - start_us) / 1000));
    return s_ready;
}

bool qso_log_store_ready() { return s_ready; }

const QsoStore* qso_log_store() { return s_ready ? &s_store : nullptr; }

QsoStoreStatus qso_log_store_add(const std::string& record, uint32_t* id_out)
{
    if (!s_ready) return QsoStoreStatus::WRITE_FAILED;
    return qso_store_add(&s_store, record.data(), record.size(), 0, id_out);
}

size_t qso_log_store_page(const QsoStoreFilter& filter, const QsoStoreCursor& before, size_t take,
                          std::vector<QsoLogRecord>* out, QsoStoreSummary* summary_out)
{
    out->clear();
    if (!s_ready) return 0;
    std::vector<uint32_t> ids(take);
    size_t matched = 0;
    const size_t n = qso_store_page(s_store, filter, before, ids.data(), ids.size(), &matched, summary_out);
    std::string fields;
    for (size_t i = 0; i < n; ++i) {
        const QsoStoreEntry* e = qso_store_entry(s_store, ids[i]);
        if (e != nullptr && qso_store_get(s_store, ids[i], &fields)) {
            out->push_back({ids[i], e->date, (e->flags & kQsoStoreForeign) != 0, fields});
        }
    }
    return matched;
}

const char* qso_log_store_band_name(uint8_t band) { return qso_store_band_name(s_store, band); }
const char* qso_log_store_mode_name(uint8_t mode) { return qso_store_mode_name(s_store, mode); }

QsoStoreStatus qso_log_store_import(const std::string& fields)
{
    if (!s_ready) return QsoStoreStatus::WRITE_FAILED;
    return qso_store_import_kept(&s_store, fields.data(), fields.size(), kQsoLogImportOwnTakes, nullptr, nullptr);
}

QsoStoreStatus qso_log_store_set(uint32_t id, const std::string& fields)
{
    if (!s_ready) return QsoStoreStatus::WRITE_FAILED;
    return qso_store_patch(&s_store, id, fields.data(), fields.size());
}

const char* qso_log_store_delete_day(uint32_t date)
{
    if (!s_ready) return "log unavailable";
    if (date == 0) return "not a daily log";
    QsoStoreFilter filter;
    filter.date = date;
    filter.whose = QsoStoreWhose::OWN;
    size_t matched = 0;
    qso_store_select(s_store, filter, 0, nullptr, 0, &matched);
    if (matched == 0) return "no such file";
    std::vector<uint32_t> ids(matched);
    qso_store_select(s_store, filter, 0, ids.data(), ids.size(), nullptr);
    for (uint32_t id : ids) {
        if (!qso_store_ok(qso_store_delete(&s_store, id))) return "delete failed";
    }
    ESP_LOGI(TAG, "deleted the %u QSOs of %08" PRIu32, (unsigned)ids.size(), date);
    return nullptr;
}

const char* qso_log_store_clear()
{
    if (!storage_service_firmware_available()) return "storage unavailable";
    QsoStore fresh;
    if (qso_store_create(&fresh, buffers(), new_store_id(), append_in_ram, nullptr) != QsoStoreStatus::OK) {
        return "log clear failed";
    }
    s_store = fresh;
    if (!storage_file_write_atomic(kQsoLogJournal, journal_bytes())) {
        s_ready = false;
        ESP_LOGE(TAG, "%s could not be rewritten; the station will not log until it is", kQsoLogJournal);
        return "log clear failed";
    }
    s_store.append = append_journal;
    s_ready = true;
    remove_daily_files();
    ESP_LOGW(TAG, "every QSO deleted: %s started again", kQsoLogJournal);
    return nullptr;
}
