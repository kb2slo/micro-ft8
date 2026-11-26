// Fills a QSO store with synthetic QSOs written by the firmware's own ADIF formatter, then times what the
// callsign card and its neighbours would ask of it (README.md). Deterministic: the same records every run, on
// every board.

#include "bench_core.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adif.h"

namespace {

// xorshift32: the same sequence on every compiler.
struct Rng {
    uint32_t s;
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    uint32_t below(uint32_t n) { return next() % n; }
    double unit() { return (next() >> 8) * (1.0 / 16777216.0); }
};

const char* const kPrefixes[] = {"K", "W", "N", "AA", "KB", "VE", "G", "DL", "JA", "EA", "F", "I", "PY", "VK", "ZL"};
constexpr uint32_t kPrefixCount = sizeof(kPrefixes) / sizeof(kPrefixes[0]);

const char* const kBands[] = {"160M", "80M", "40M", "30M", "20M", "17M", "15M", "12M", "10M", "6M"};
const char* const kFreqs[] = {"1.840", "3.573", "7.074", "10.136", "14.074", "18.100", "21.074", "24.915", "28.074", "50.313"};
// Weights per band: most FT8 is on 40, 20 and 15 m.
const uint32_t kBandWeight[] = {2, 6, 20, 8, 30, 8, 12, 4, 8, 2};

// A distinct call for each station index: prefix, digit, then at least two letters.
std::string station_call(uint32_t index)
{
    std::string call = kPrefixes[index % kPrefixCount];
    call += (char)('0' + (index / kPrefixCount) % 10);
    uint32_t rest = index / (kPrefixCount * 10);
    std::string suffix;
    do {
        suffix.insert(suffix.begin(), (char)('A' + rest % 26));
        rest /= 26;
    } while (rest != 0);
    while (suffix.size() < 2) suffix.insert(suffix.begin(), 'A');
    return call + suffix;
}

// A station's grid follows from its index, so every QSO with it carries the same one.
std::string station_grid(uint32_t index)
{
    const uint32_t h = index * 2654435761u;
    char grid[5] = {(char)('A' + h % 18), (char)('A' + (h / 18) % 18), (char)('0' + (h / 324) % 10),
                    (char)('0' + (h / 3240) % 10), 0};
    return grid;
}

// Days since 1970-01-01 to YYYYMMDD (Howard Hinnant's civil_from_days).
uint32_t ymd(int32_t days)
{
    days += 719468;
    const int32_t era = (days >= 0 ? days : days - 146096) / 146097;
    const uint32_t doe = (uint32_t)(days - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t y = (int32_t)yoe + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    return (uint32_t)(y + (m <= 2)) * 10000 + m * 100 + d;
}

bool append_nothing(void*, const uint8_t*, size_t) { return true; }  // the image already holds the bytes

struct Truth {
    std::vector<uint32_t> heavy_ids;    // every record with the most-worked call, oldest first
    std::vector<uint32_t> typical_ids;  // with a call worked a typical number of times
    std::string heavy_call;
    std::string typical_call;
    std::string heavy_grid;
    uint32_t heavy_grid_records = 0;  // records whose grid is the heavy call's: that one and any sharing it
    uint32_t heavy_20m_ft8_newest = 0;
    uint32_t heavy_at_park_newest = 0;
    uint32_t mid_date = 0;
    uint32_t mid_date_records = 0;
    uint32_t stations = 0;
};

// N records over three years, oldest first as a log grows. A third as many stations as QSOs, and a skewed
// pick (u cubed), so a few stations are worked dozens of times and most once or twice.
QsoStoreStatus fill(QsoStore* store, const BenchMemory& m, uint32_t n, Truth* truth, int64_t* add_us_out,
                    BenchClockFn now_us)
{
    QsoStoreBuffers buffers{m.image, m.image_bytes, m.entries, m.entry_count, m.call_slots, m.call_slot_count};
    QsoStoreStatus st = qso_store_create(store, buffers, 0x5EED, append_nothing, nullptr);
    if (st != QsoStoreStatus::OK) return st;
    Rng rng{0x1234567u + n};
    const uint32_t stations = n / 3 > 1 ? n / 3 : 1;
    std::vector<uint32_t> per_station(stations, 0);
    std::vector<uint32_t> picks(n);
    for (uint32_t i = 0; i < n; ++i) {
        const double u = rng.unit();
        picks[i] = (uint32_t)(stations * u * u * u);
        ++per_station[picks[i]];
    }
    // The typical station: the one at the median of what each QSO's station was worked, over all QSOs.
    std::vector<uint32_t> counts;
    counts.reserve(n);
    for (uint32_t i = 0; i < n; ++i) counts.push_back(per_station[picks[i]]);
    std::vector<uint32_t> sorted = counts;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const uint32_t median = sorted[n / 2];
    uint32_t typical = 0;
    while (typical < stations && per_station[typical] != median) ++typical;

    truth->stations = stations;
    truth->heavy_call = station_call(0);
    truth->typical_call = station_call(typical);
    truth->heavy_grid = station_grid(0);
    const int32_t first_day = 19723;  // 2024-01-01
    const uint32_t band_total = [] {
        uint32_t t = 0;
        for (uint32_t w : kBandWeight) t += w;
        return t;
    }();
    int64_t add_us = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t station = picks[i];
        uint32_t w = rng.below(band_total);
        uint32_t band = 0;
        while (w >= kBandWeight[band]) w -= kBandWeight[band++];
        AdifLogFields f;
        f.call = station_call(station);
        f.gridsquare = station_grid(station);
        f.mode = rng.below(10) == 0 ? "FT4" : "FT8";
        const uint32_t date = ymd(first_day + (int32_t)((uint64_t)i * 1096 / n));
        char text[16];
        snprintf(text, sizeof(text), "%08" PRIu32, date);
        f.qso_date = text;
        snprintf(text, sizeof(text), "%02u%02u%02u", (unsigned)rng.below(24), (unsigned)rng.below(60),
                 (unsigned)rng.below(60));
        f.time_on = text;
        f.freq = kFreqs[band];
        f.band = kBands[band];
        f.station_callsign = "N0CALL";
        f.my_gridsquare = "EM10";
        f.rst_sent = -24 + (int)rng.below(35);
        f.rst_rcvd = -24 + (int)rng.below(35);
        if (rng.below(2) == 0) f.comment = "QMX";
        const bool at_park = rng.below(10) == 0;
        if (at_park) f.my_pota_ref = "US-1234";
        const std::string line = adif_format_log_record(f);
        uint32_t id = 0;
        const int64_t t0 = now_us();
        st = qso_store_add(store, line.data(), line.size(), 0, &id);
        add_us += now_us() - t0;
        if (st != QsoStoreStatus::OK) return st;
        if (station == 0) {
            truth->heavy_ids.push_back(id);
            if (band == 4 && f.mode == "FT8") truth->heavy_20m_ft8_newest = id;
            if (at_park) truth->heavy_at_park_newest = id;
        }
        if (station == typical) truth->typical_ids.push_back(id);
        if (f.gridsquare == truth->heavy_grid) ++truth->heavy_grid_records;
        if (i == n / 2) truth->mid_date = date;
        if (truth->mid_date != 0 && date == truth->mid_date) ++truth->mid_date_records;
    }
    // Records on the middle day logged before record n/2 was reached.
    for (uint32_t i = 0; i < n / 2; ++i) {
        if (ymd(first_day + (int32_t)((uint64_t)i * 1096 / n)) == truth->mid_date) ++truth->mid_date_records;
    }
    *add_us_out = add_us;
    return QsoStoreStatus::OK;
}

void result(const char* where, uint32_t n, const char* op, uint32_t hits, int reps, int64_t total_us)
{
    printf("RESULT where=%s n=%" PRIu32 " op=%s hits=%" PRIu32 " reps=%d us=%.1f\n", where, n, op, hits, reps,
           (double)total_us / reps);
}

// Every record with `call`, as the card would want them: the probe, then each record's fields.
uint32_t by_call(const QsoStore& store, const char* call, const std::vector<uint32_t>& ids, std::string* fields)
{
    uint32_t newest = 0;
    qso_store_worked(store, call, nullptr, nullptr, 0, nullptr, &newest);
    uint32_t got = 0;
    for (uint32_t id : ids) got += qso_store_get(store, id, fields) ? 1 : 0;
    return newest != 0 ? got : 0;
}

bool run_size(const BenchMemory& m, BenchClockFn now_us, const char* where, uint32_t n)
{
    static QsoStore store;  // big enough that the device's main task stack should not hold it
    Truth truth;
    int64_t add_us = 0;
    const QsoStoreStatus st = fill(&store, m, n, &truth, &add_us, now_us);
    if (st != QsoStoreStatus::OK) {
        printf("CHECK where=%s n=%" PRIu32 " FAIL fill status=%d\n", where, n, (int)st);
        return false;
    }
    bool ok = true;
    const size_t journal = store.end;
    printf("RESULT where=%s n=%" PRIu32 " op=size stations=%" PRIu32 " journal_bytes=%u bytes_per_record=%.1f\n",
           where, n, truth.stations, (unsigned)journal, (double)(journal - kQsoStoreHeaderBytes) / n);
    result(where, n, "add", n, (int)n, add_us);

    // Boot's work once the file is in RAM: checking every entry and rebuilding the index.
    {
        QsoStoreBuffers buffers{m.image, m.image_bytes, m.entries, m.entry_count, m.call_slots, m.call_slot_count};
        const int reps = 3;
        int64_t total = 0;
        QsoStoreLoad load{};
        for (int r = 0; r < reps; ++r) {
            const int64_t t0 = now_us();
            load = qso_store_load(&store, buffers, journal, append_nothing, nullptr);
            total += now_us() - t0;
        }
        ok = ok && load.status == QsoStoreStatus::OK && load.valid_bytes == journal && load.damaged_bytes == 0;
        result(where, n, "load", (uint32_t)store.entries_used, reps, total);
    }

    std::string fields;
    fields.reserve(512);
    const int reps = 200;
    auto time_by_call = [&](const char* op, const std::string& call, const std::vector<uint32_t>& ids) {
        uint32_t hits = 0;
        const int64_t t0 = now_us();
        for (int r = 0; r < reps; ++r) hits = by_call(store, call.c_str(), ids, &fields);
        result(where, n, op, hits, reps, now_us() - t0);
        ok = ok && hits == ids.size();
        // The probe alone, which is what tells a call apart from the records' reading.
        uint32_t newest = 0;
        const int64_t t1 = now_us();
        for (int r = 0; r < reps; ++r) qso_store_worked(store, call.c_str(), nullptr, nullptr, 0, nullptr, &newest);
        result(where, n, (std::string(op) + "_probe").c_str(), newest != 0 ? 1 : 0, reps, now_us() - t1);
        ok = ok && newest == ids.back();
    };
    time_by_call("by_call_heavy", truth.heavy_call, truth.heavy_ids);
    time_by_call("by_call_typical", truth.typical_call, truth.typical_ids);
    {
        uint32_t newest = 0;
        bool found = false;
        const int64_t t0 = now_us();
        for (int r = 0; r < reps; ++r) found = qso_store_worked(store, "ZZ9ZZZ", nullptr, nullptr, 0, nullptr, &newest);
        result(where, n, "by_call_miss_probe", found ? 1 : 0, reps, now_us() - t0);
        ok = ok && !found;
    }
    {
        uint32_t newest = 0;
        const int64_t t0 = now_us();
        for (int r = 0; r < reps; ++r) qso_store_worked(store, truth.heavy_call.c_str(), "20M", "FT8", 0, nullptr, &newest);
        result(where, n, "worked_band_mode", newest != 0 ? 1 : 0, reps, now_us() - t0);
        ok = ok && newest == truth.heavy_20m_ft8_newest;
    }
    {
        // During an activation the check reads each candidate's fields.
        uint32_t newest = 0;
        const int64_t t0 = now_us();
        for (int r = 0; r < reps; ++r) {
            newest = 0;
            qso_store_worked(store, truth.heavy_call.c_str(), nullptr, nullptr, 0, "POTA:US-1234", &newest);
        }
        result(where, n, "worked_at_park", newest != 0 ? 1 : 0, reps, now_us() - t0);
        ok = ok && newest == truth.heavy_at_park_newest;
    }

    // Passes over the index.
    std::vector<uint32_t> ids(50);
    {
        QsoStoreFilter filter;
        filter.date = truth.mid_date;
        size_t matched = 0;
        const int64_t t0 = now_us();
        for (int r = 0; r < 20; ++r) qso_store_select(store, filter, 0, ids.data(), ids.size(), &matched);
        result(where, n, "select_date", (uint32_t)matched, 20, now_us() - t0);
        ok = ok && matched == truth.mid_date_records;
    }
    {
        QsoStoreFilter filter;
        filter.unsent = true;
        filter.newest_first = true;
        size_t matched = 0;
        const int64_t t0 = now_us();
        for (int r = 0; r < 20; ++r) qso_store_select(store, filter, 0, ids.data(), ids.size(), &matched);
        result(where, n, "select_unsent", (uint32_t)matched, 20, now_us() - t0);
        ok = ok && matched == n;
    }

    // A field with no index: every record read through the store, as a grid search would be today.
    const std::string tag = "<gridsquare:4>" + truth.heavy_grid;
    {
        std::vector<uint32_t> all(store.entries_used);
        const size_t count = qso_store_select(store, QsoStoreFilter{}, 0, all.data(), all.size(), nullptr);
        uint32_t hits = 0;
        const int reps_scan = 3;
        const int64_t t0 = now_us();
        for (int r = 0; r < reps_scan; ++r) {
            hits = 0;
            for (size_t i = 0; i < count; ++i) {
                if (qso_store_get(store, all[i], &fields) && fields.find(tag) != std::string::npos) ++hits;
            }
        }
        result(where, n, "scan_grid_get", hits, reps_scan, now_us() - t0);
        ok = ok && hits == truth.heavy_grid_records;
    }
    {
        // The floor under any such scan: one pass over the journal's bytes, reading nothing into a string.
        uint32_t hits = 0;
        const int reps_scan = 3;
        const int64_t t0 = now_us();
        for (int r = 0; r < reps_scan; ++r) {
            hits = 0;
            const uint8_t* p = m.image;
            const uint8_t* end = m.image + journal;
            while (p < end) {
                const uint8_t* hit = (const uint8_t*)memmem(p, (size_t)(end - p), tag.data(), tag.size());
                if (hit == nullptr) break;
                ++hits;
                p = hit + tag.size();
            }
        }
        result(where, n, "scan_grid_raw", hits, reps_scan, now_us() - t0);
        ok = ok && hits == truth.heavy_grid_records;
    }

    printf("CHECK where=%s n=%" PRIu32 " %s heavy=%s:%u typical=%s:%u\n", where, n, ok ? "ok" : "FAIL",
           truth.heavy_call.c_str(), (unsigned)truth.heavy_ids.size(), truth.typical_call.c_str(),
           (unsigned)truth.typical_ids.size());
    return ok;
}

}  // namespace

bool bench_run(const BenchMemory& memory, BenchClockFn now_us, const char* where)
{
    printf("BENCH begin where=%s image_bytes=%u entries=%u call_slots=%u\n", where, (unsigned)memory.image_bytes,
           (unsigned)memory.entry_count, (unsigned)memory.call_slot_count);
    bool ok = true;
    // Today's log, a few years of a busy station, and about what the free flash would hold.
    for (uint32_t n : {1000u, 8000u, 19000u}) ok = run_size(memory, now_us, where, n) && ok;
    printf("BENCH done where=%s %s\n", where, ok ? "ok" : "FAIL");
    return ok;
}
