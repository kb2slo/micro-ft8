// qso_store: the journal's bytes, what a load recovers from a write cut short and from damage, and the
// queries (components/qso_store/include/qso_store.h).
//
// The store is checked against a second implementation of the journal's meaning, written here from the
// header's description: replay() applies the changes qso_store_feed() hands out, as a browser's copy would.
// A store loaded from a journal with some entries lost must then equal a replay without those entries.

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "qso_store.h"

static int g_fails = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                 \
            fprintf(stderr, "\n");                        \
            ++g_fails;                                    \
        }                                                 \
    } while (0)

// The journal's file: what the append function has been given.
struct Disk {
    std::vector<uint8_t> bytes;
    bool fail = false;
};

static bool disk_append(void* ctx, const uint8_t* bytes, size_t len)
{
    Disk* disk = (Disk*)ctx;
    if (disk->fail) return false;
    disk->bytes.insert(disk->bytes.end(), bytes, bytes + len);
    return true;
}

struct Rig {
    std::vector<uint8_t> image;
    std::vector<QsoStoreEntry> entries;
    std::vector<uint32_t> slots;
    QsoStore store{};

    Rig(size_t image_bytes, size_t entry_count, size_t slot_count)
        : image(image_bytes), entries(entry_count), slots(slot_count) {}
    Rig() : Rig(64 * 1024, 256, 512) {}

    QsoStoreBuffers buffers()
    {
        return {image.data(), image.size(), entries.data(), entries.size(), slots.data(), slots.size()};
    }
    QsoStoreLoad load(Disk* disk)
    {
        if (!disk->bytes.empty()) memcpy(image.data(), disk->bytes.data(), disk->bytes.size());
        return qso_store_load(&store, buffers(), disk->bytes.size(), disk_append, disk);
    }
};

typedef std::map<std::string, std::string> Fields;

struct Rec {
    Fields fields;
    uint8_t flags = 0;
};

struct World {
    std::map<uint32_t, Rec> recs;
    Fields meta;
    uint32_t max_id = 0;
};

struct Change {
    QsoStoreChangeKind kind;
    uint8_t flags;
    uint32_t id;
    std::string fields;
    size_t at;
    size_t size;
};

// `<name:length>value` fields as the store writes them; names are already lower case.
static std::vector<std::pair<std::string, std::string>> parse(const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> out;
    size_t at = 0;
    while (at < text.size() && text[at] == '<') {
        const size_t colon = text.find(':', at);
        const size_t close = text.find('>', at);
        const size_t len = (size_t)atoi(text.c_str() + colon + 1);
        out.push_back({text.substr(at + 1, colon - at - 1), text.substr(close + 1, len)});
        at = close + 1 + len;
    }
    return out;
}

static Fields as_map(const std::string& text)
{
    Fields out;
    for (const auto& f : parse(text)) out[f.first] = f.second;
    return out;
}

static std::vector<Change> read_feed(const QsoStore& store, size_t cursor = 0)
{
    std::vector<Change> out;
    QsoStoreChange c;
    for (;;) {
        const QsoStoreFeed got = qso_store_feed(store, &cursor, &c);
        if (got != QsoStoreFeed::CHANGE) {
            CHECK(got == QsoStoreFeed::CAUGHT_UP && cursor == store.end,
                  "the feed ends caught up, at the journal's end");
            return out;
        }
        const size_t at = (size_t)((const uint8_t*)c.fields - store.buffers.image) - 16;  // its header is 16 bytes
        out.push_back({c.kind, c.flags, c.id, std::string(c.fields, c.len), at, kQsoStoreEntryOverhead + c.len});
    }
}

static World replay(const std::vector<Change>& changes, const std::set<size_t>& lost = {})
{
    World w;
    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& c = changes[i];
        if (lost.count(i)) continue;
        switch (c.kind) {
            case QsoStoreChangeKind::ADDED:
                if (c.id <= w.max_id) break;
                w.max_id = c.id;
                w.recs[c.id].fields = as_map(c.fields);
                w.recs[c.id].flags = c.flags;
                break;
            case QsoStoreChangeKind::FIELDS_SET:
                if (!w.recs.count(c.id)) break;
                for (const auto& f : parse(c.fields)) {
                    if (f.second.empty()) w.recs[c.id].fields.erase(f.first);
                    else w.recs[c.id].fields[f.first] = f.second;
                }
                break;
            case QsoStoreChangeKind::DELETED:
                w.recs.erase(c.id);
                break;
            case QsoStoreChangeKind::META: {
                const size_t eq = c.fields.find('=');
                w.meta[c.fields.substr(0, eq)] = c.fields.substr(eq + 1);
                break;
            }
        }
    }
    return w;
}

static std::string upper(std::string s)
{
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static std::string mode_of(const Fields& f)
{
    if (f.count("submode")) return upper(f.at("submode"));
    return f.count("mode") ? upper(f.at("mode")) : "";
}

// Everything the store answers, against a world.
static void verify(const QsoStore& store, const World& w, const char* what)
{
    std::string got;
    size_t unsent_own = 0;
    std::map<std::string, uint32_t> newest;  // by call, band and mode: the map is in ID order
    auto key_of = [](const Fields& f) {
        return upper(f.at("call")) + "|" + upper(f.count("band") ? f.at("band") : "") + "|" + mode_of(f);
    };
    for (const auto& r : w.recs) newest[key_of(r.second.fields)] = r.first;
    for (uint32_t id = 1; id < store.next_id + 2; ++id) {
        const bool held = qso_store_get(store, id, &got);
        const QsoStoreEntry* entry = qso_store_entry(store, id);
        CHECK(held == (w.recs.count(id) != 0) && held == (entry != nullptr), "%s: record %u is %s", what,
              (unsigned)id, w.recs.count(id) ? "missing" : "there and should not be");
        if (!held || !w.recs.count(id) || entry == nullptr) continue;
        const Rec& rec = w.recs.at(id);
        CHECK(as_map(got) == rec.fields, "%s: record %u reads %s", what, (unsigned)id, got.c_str());
        CHECK(entry->flags == rec.flags, "%s: record %u flags", what, (unsigned)id);
        CHECK(upper(rec.fields.at("call")) == entry->call,
              "%s: record %u indexed as %s", what, (unsigned)id, entry->call);
        const uint64_t logid = rec.fields.count("app_qrzlog_logid")
                                   ? strtoull(rec.fields.at("app_qrzlog_logid").c_str(), nullptr, 10)
                                   : 0;
        CHECK(entry->qrz_logid == logid, "%s: record %u QRZ ID", what, (unsigned)id);
        const bool uploaded = rec.fields.count("qrzcom_qso_upload_status") &&
                              upper(rec.fields.at("qrzcom_qso_upload_status")) == "Y";
        CHECK(entry->uploaded == uploaded, "%s: record %u marked uploaded", what, (unsigned)id);
        if (logid == 0 && !uploaded && rec.flags == 0) ++unsent_own;

        // Worked before, asked the way a decode asks: the newest record with this call, band and mode.
        const uint32_t want = newest.at(key_of(rec.fields));
        if (rec.fields.count("band") && !mode_of(rec.fields).empty()) {
            uint32_t worked = 0;
            const bool yes = qso_store_worked(store, rec.fields.at("call").c_str(), rec.fields.at("band").c_str(),
                                              mode_of(rec.fields).c_str(), 0, nullptr, &worked);
            CHECK(yes && worked == want, "%s: worked(%s) names %u, want %u", what, entry->call, (unsigned)worked,
                  (unsigned)want);
        }
    }
    size_t matched = 0;
    QsoStoreFilter all;
    qso_store_select(store, all, 0, nullptr, 0, &matched);
    CHECK(matched == w.recs.size(), "%s: %u records, want %u", what, (unsigned)matched, (unsigned)w.recs.size());
    QsoStoreFilter unsent;
    unsent.unsent = true;
    unsent.whose = QsoStoreWhose::OWN;
    qso_store_select(store, unsent, 0, nullptr, 0, &matched);
    CHECK(matched == unsent_own, "%s: %u unsent, want %u", what, (unsigned)matched, (unsigned)unsent_own);
    for (const auto& m : w.meta) {
        CHECK(qso_store_meta_get(store, m.first.c_str(), &got) && got == m.second,
              "%s: meta %s", what, m.first.c_str());
    }
    CHECK(store.meta_used == w.meta.size(), "%s: meta keys", what);
}

static QsoStoreStatus add(QsoStore* store, const char* fields, uint8_t flags = 0, uint32_t* id = nullptr)
{
    return qso_store_add(store, fields, strlen(fields), flags, id);
}

static QsoStoreStatus patch(QsoStore* store, uint32_t id, const char* fields)
{
    return qso_store_patch(store, id, fields, strlen(fields));
}

// The journal the Python encoder beside this file writes (qso_store_golden.py): each kind of entry once,
// CRCs from zlib.
static const uint8_t kGolden[] = {
        0x4D, 0x46, 0x51, 0x53, 0x01, 0x00, 0x18, 0x00, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
        0x00, 0x00, 0x00, 0x00, 0xDA, 0x43, 0xED, 0x7E, 0xD5, 0x01, 0x48, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x63, 0x61, 0x6C, 0x6C, 0x3A, 0x35, 0x3E,
        0x4B, 0x31, 0x41, 0x42, 0x43, 0x3C, 0x62, 0x61, 0x6E, 0x64, 0x3A, 0x33, 0x3E, 0x32, 0x30, 0x4D,
        0x3C, 0x6D, 0x6F, 0x64, 0x65, 0x3A, 0x33, 0x3E, 0x46, 0x54, 0x38, 0x3C, 0x71, 0x73, 0x6F, 0x5F,
        0x64, 0x61, 0x74, 0x65, 0x3A, 0x38, 0x3E, 0x32, 0x30, 0x32, 0x36, 0x31, 0x31, 0x31, 0x39, 0x3C,
        0x74, 0x69, 0x6D, 0x65, 0x5F, 0x6F, 0x6E, 0x3A, 0x36, 0x3E, 0x32, 0x31, 0x35, 0x34, 0x30, 0x30,
        0x90, 0xD7, 0x61, 0x45, 0xD5, 0x02, 0x1D, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x3C, 0x61, 0x70, 0x70, 0x5F, 0x71, 0x72, 0x7A, 0x6C, 0x6F, 0x67, 0x5F,
        0x6C, 0x6F, 0x67, 0x69, 0x64, 0x3A, 0x39, 0x3E, 0x31, 0x33, 0x30, 0x38, 0x37, 0x37, 0x38, 0x32,
        0x35, 0x78, 0xD2, 0x63, 0xC4, 0xD5, 0x04, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x71, 0x72, 0x7A, 0x5F, 0x6D, 0x6F, 0x64, 0x73, 0x69, 0x6E, 0x63,
        0x65, 0x3D, 0x32, 0x30, 0x32, 0x36, 0x2D, 0x31, 0x31, 0x2D, 0x31, 0x39, 0x71, 0xB9, 0xEB, 0x84,
        0xD5, 0x01, 0x21, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x3C, 0x63, 0x61, 0x6C, 0x6C, 0x3A, 0x34, 0x3E, 0x57, 0x39, 0x58, 0x59, 0x3C, 0x61, 0x70, 0x70,
        0x5F, 0x71, 0x72, 0x7A, 0x6C, 0x6F, 0x67, 0x5F, 0x6C, 0x6F, 0x67, 0x69, 0x64, 0x3A, 0x31, 0x3E,
        0x37, 0xE5, 0xA2, 0xF5, 0x6B, 0xD5, 0x03, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xD0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xEF, 0xE5, 0x0D, 0x64,
};

static void test_golden_bytes()
{
    Rig rig;
    Disk disk;
    uint32_t id = 0;
    CHECK(qso_store_create(&rig.store, rig.buffers(), 0x0123456789ABCDEFull, disk_append, &disk) == QsoStoreStatus::OK,
          "create");
    // As the logger writes a record: upper-case names would do as well, spaces between fields, <eor>.
    CHECK(add(&rig.store, "<CALL:5>K1ABC <band:3>20M <mode:3>FT8 "
                          "<qso_date:8>20261119 <time_on:6>215400 <eor>\n", 0, &id) == QsoStoreStatus::OK &&
          id == 1,
          "add");
    CHECK(patch(&rig.store, 1, "<app_qrzlog_logid:9>130877825") == QsoStoreStatus::OK, "patch");
    CHECK(qso_store_meta_set(&rig.store, "qrz_modsince", "2026-11-19") == QsoStoreStatus::OK, "meta");
    CHECK(qso_store_import(&rig.store, "<call:4>W9XY<app_qrzlog_logid:1>7", 33, &id) == QsoStoreStatus::OK && id == 2,
          "import");
    CHECK(qso_store_delete(&rig.store, 2) == QsoStoreStatus::OK, "delete");
    CHECK(disk.bytes.size() == sizeof kGolden && memcmp(disk.bytes.data(), kGolden, sizeof kGolden) == 0,
          "the journal is byte for byte what qso_store_golden.py writes");

    // And read back from those bytes alone.
    Rig again;
    Disk golden;
    golden.bytes.assign(kGolden, kGolden + sizeof kGolden);
    const QsoStoreLoad load = again.load(&golden);
    std::string got;
    CHECK(load.status == QsoStoreStatus::OK && load.valid_bytes == sizeof kGolden && load.damaged_bytes == 0 &&
          load.ignored == 0,
          "the golden journal loads whole");
    CHECK(again.store.store_id == 0x0123456789ABCDEFull && again.store.next_id == 3,
          "identity and the next ID survive a load");
    CHECK(qso_store_get(again.store, 1, &got) &&
          as_map(got) == as_map("<call:5>K1ABC<band:3>20M<mode:3>FT8<qso_date:8>20261119<time_on:6>215400"
                                 "<app_qrzlog_logid:9>130877825"),
          "record 1 reads back: %s", got.c_str());
    CHECK(!qso_store_get(again.store, 2, &got) && qso_store_entry(again.store, 2) == nullptr, "record 2 stays deleted");
    const QsoStoreEntry* entry = qso_store_entry(again.store, 1);
    CHECK(entry && entry->date == 20261119 && entry->time == 2154 && entry->qrz_logid == 130877825 && entry->flags == 0,
          "record 1's index entry");
}

static void test_fields()
{
    Rig rig;
    Disk disk;
    uint32_t id = 0;
    std::string got;
    qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
    CHECK(add(&rig.store, "<call:5>k1abc <GRIDSQUARE:0> <Band:3:E>20m "
                          "<comment:12>5W <eor> ant <mode:3>FT8", 0, &id) == QsoStoreStatus::OK,
          "a record with a type indicator, an empty field and a value holding <eor>");
    CHECK(qso_store_get(rig.store, id, &got) && got == "<call:5>k1abc<band:3>20m<comment:12>5W <eor> ant<mode:3>FT8",
          "kept as %s", got.c_str());
    CHECK(strcmp(qso_store_entry(rig.store, id)->call, "K1ABC") == 0, "the index holds the call in upper case");
    CHECK(add(&rig.store, "<call:5>W1DUP<band:3>20M<mode:3>FT8<BAND:3>40M", 0, &id) == QsoStoreStatus::OK &&
              qso_store_get(rig.store, id, &got) && got == "<call:5>W1DUP<mode:3>FT8<band:3>40M",
          "a field named twice is kept once, with its last value: %s", got.c_str());
    CHECK(qso_store_worked(rig.store, "W1DUP", "40M", "FT8", 0, nullptr, nullptr) &&
              !qso_store_worked(rig.store, "W1DUP", "20M", "FT8", 0, nullptr, nullptr),
          "and indexed by it");
    const size_t end = rig.store.end;
    CHECK(add(&rig.store, "<band:3>20M<mode:3>FT8") == QsoStoreStatus::BAD_FIELDS, "no CALL");
    CHECK(add(&rig.store, "<call:0><band:3>20M") == QsoStoreStatus::BAD_FIELDS, "an empty CALL");
    CHECK(add(&rig.store, "<call:16>K1ABCDEFGHIJKLMN") == QsoStoreStatus::BAD_FIELDS, "a CALL too long for the index");
    CHECK(add(&rig.store, "<call:9>K1ABC") == QsoStoreStatus::BAD_FIELDS, "a length past the end of the text");
    CHECK(add(&rig.store, "<call:5>K1ABC<band>20M") == QsoStoreStatus::BAD_FIELDS, "a field with no length");
    CHECK(add(&rig.store, "<call:5>K1ABC<band:x>20M") == QsoStoreStatus::BAD_FIELDS, "a length that is not a number");
    CHECK(add(&rig.store, "<call:5>K1ABC<band:99999999999999999999>20M") == QsoStoreStatus::BAD_FIELDS,
          "a length that overflows");
    const std::string longer = "<call:5>K1ABC<comment:1010>" + std::string(1010, 'x');
    CHECK(add(&rig.store, longer.c_str()) == QsoStoreStatus::TOO_LONG, "more fields than an entry holds");
    CHECK(rig.store.end == end && disk.bytes.size() == end && rig.store.next_id == id + 1,
          "a refused record writes nothing and takes no ID");
}

static void test_patch_delete_worked()
{
    Rig rig;
    Disk disk;
    uint32_t a = 0, b = 0, c = 0, d = 0, got_id = 0;
    std::string got;
    qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
    add(&rig.store, "<call:5>K1ABC<band:3>20M<mode:3>FT8<qso_date:8>20261101<time_on:6>120000", 0, &a);
    add(&rig.store, "<call:5>K1ABC<band:3>40M<mode:4>MFSK<submode:3>FT4<qso_date:8>20261110<time_on:6>130000", 0, &b);
    add(&rig.store, "<call:4>W9XY<band:3>20M<mode:3>FT8<qso_date:8>20261110<time_on:6>130100", 0, &c);
    add(&rig.store, "<call:5>K1ABC<band:3>20M<mode:3>FT8<qso_date:8>20261119<time_on:6>215400<app_qrzlog_logid:2>55",
        kQsoStoreForeign, &d);

    CHECK(qso_store_worked(rig.store, "k1abc", "20m", "ft8", 0, nullptr, &got_id) && got_id == d,
          "worked names the newest match, own or foreign, whatever the case");
    CHECK(qso_store_worked(rig.store, "K1ABC", "40M", "FT4", 0, nullptr, &got_id) && got_id == b,
          "a record logged MFSK with SUBMODE FT4 is FT4");
    CHECK(!qso_store_worked(rig.store, "K1ABC", "40M", "MFSK", 0, nullptr, nullptr), "and is not MFSK");
    CHECK(!qso_store_worked(rig.store, "K1ABC", "40M", "FT8", 0, nullptr, nullptr),
          "another mode on the band is not worked");
    CHECK(!qso_store_worked(rig.store, "K1ABC", "15M", "FT8", 0, nullptr, nullptr), "a band no record is on");
    CHECK(qso_store_worked(rig.store, "K1ABC", nullptr, nullptr, 0, nullptr, &got_id) && got_id == d,
          "any band and mode");
    CHECK(qso_store_worked(rig.store, "K1ABC", "40M", nullptr, 20261110, nullptr, &got_id) && got_id == b,
          "on or after a date");
    CHECK(!qso_store_worked(rig.store, "K1ABC", "40M", nullptr, 20261111, nullptr, nullptr), "not before it");
    CHECK(!qso_store_worked(rig.store, "K1AB", nullptr, nullptr, 0, nullptr, nullptr) &&
          !qso_store_worked(rig.store, "K1ABCD", nullptr, nullptr, 0, nullptr, nullptr),
          "a call is matched whole");

    // Fields set, removed and left alone.
    size_t end = rig.store.end;
    CHECK(patch(&rig.store, a, "<band:3>20M<mode:3>FT8") == QsoStoreStatus::UNCHANGED && rig.store.end == end,
          "a patch that says what the record says writes nothing");
    CHECK(patch(&rig.store, a, "<gridsquare:0>") == QsoStoreStatus::UNCHANGED && rig.store.end == end,
          "nor does removing a field it lacks");
    CHECK(patch(&rig.store, a, "<band:3>20M<APP_QRZLOG_LOGID:3>777<comment:2>hi") == QsoStoreStatus::OK,
          "set two fields");
    CHECK(rig.store.end == end + kQsoStoreEntryOverhead + strlen("<app_qrzlog_logid:3>777<comment:2>hi"),
          "only the fields that changed are written");
    CHECK(qso_store_find_qrz(rig.store, 777, &got_id) && got_id == a, "found by QRZ's ID");
    CHECK(patch(&rig.store, a, "<comment:0>") == QsoStoreStatus::OK && qso_store_get(rig.store, a, &got) &&
          !as_map(got).count("comment"),
          "a field of no length removes it");
    CHECK(as_map(got).at("app_qrzlog_logid") == "777" && as_map(got).at("band") == "20M", "and leaves the others");
    CHECK(patch(&rig.store, a, "<call:0>") == QsoStoreStatus::BAD_FIELDS, "the CALL cannot be removed");
    CHECK(patch(&rig.store, 99, "<comment:2>hi") == QsoStoreStatus::NOT_FOUND, "no such record");

    // An edit that moves a record to another call, band and mode moves what worked() says.
    CHECK(patch(&rig.store, c, "<call:6>N0CALL<band:3>40M<submode:3>FT4") == QsoStoreStatus::OK,
          "edit call, band and mode");
    CHECK(!qso_store_worked(rig.store, "W9XY", nullptr, nullptr, 0, nullptr, nullptr),
          "the old call is no longer worked");
    CHECK(qso_store_worked(rig.store, "N0CALL", "40M", "FT4", 0, nullptr, &got_id) && got_id == c, "the new one is");
    CHECK(patch(&rig.store, c, "<submode:0>") == QsoStoreStatus::OK &&
          qso_store_worked(rig.store, "N0CALL", "40M", "FT8", 0, nullptr, nullptr),
          "with SUBMODE gone the mode is MODE again");

    // Select: a day, whose, unsent, paging, both orders.
    uint32_t ids[8];
    size_t matched = 0;
    QsoStoreFilter day;
    day.date = 20261110;
    CHECK(qso_store_select(rig.store, day, 0, ids, 8, &matched) == 2 && matched == 2 && ids[0] == b && ids[1] == c,
          "a day, oldest first");
    day.newest_first = true;
    CHECK(qso_store_select(rig.store, day, 1, ids, 8, &matched) == 1 && matched == 2 && ids[0] == b,
          "newest first, after the first");
    QsoStoreFilter unsent;
    unsent.unsent = true;
    unsent.whose = QsoStoreWhose::OWN;
    CHECK(qso_store_select(rig.store, unsent, 0, ids, 8, &matched) == 2 && ids[0] == b && ids[1] == c,
          "own records QRZ has given no ID");
    CHECK(qso_store_select(rig.store, unsent, 0, ids, 1, &matched) == 1 && matched == 2,
          "a short page still says how many match");
    // QRZ refuses a QSO it already holds and gives no ID for it: the record is marked uploaded all the same.
    CHECK(patch(&rig.store, b, "<qrzcom_qso_upload_status:1>y") == QsoStoreStatus::OK &&
              qso_store_select(rig.store, unsent, 0, ids, 8, &matched) == 1 && ids[0] == c,
          "a record marked uploaded is not unsent, ID or no ID");
    CHECK(patch(&rig.store, b, "<qrzcom_qso_upload_status:1>N") == QsoStoreStatus::OK &&
              qso_store_select(rig.store, unsent, 0, ids, 8, &matched) == 2,
          "and is unsent again when the mark says no");
    QsoStoreFilter foreign;
    foreign.whose = QsoStoreWhose::FOREIGN;
    CHECK(qso_store_select(rig.store, foreign, 0, ids, 8, nullptr) == 1 && ids[0] == d, "foreign records");

    // During an activation only a QSO logged at one of the station's references counts.
    {
        Rig park;
        Disk park_disk;
        uint32_t home = 0, one = 0, two = 0, summit = 0;
        qso_store_create(&park.store, park.buffers(), 1, disk_append, &park_disk);
        add(&park.store, "<call:5>K1ABC<band:3>20M<mode:3>FT8<qso_date:8>20261119", 0, &home);
        add(&park.store, "<call:4>W9XY<band:3>20M<mode:3>FT8<qso_date:8>20261119<my_pota_ref:7>US-1234", 0, &one);
        add(&park.store, "<call:4>AA1A<band:3>20M<mode:3>FT8<qso_date:8>20261119<my_pota_ref:16>us-1234, US-5678",
            0, &two);
        add(&park.store, "<call:4>AA1A<band:3>20M<mode:3>FT8<qso_date:8>20261119<my_sota_ref:10>W7O/CC-001",
            0, &summit);
        const QsoStore& s = park.store;
        CHECK(qso_store_worked(s, "K1ABC", "20M", "FT8", 0, nullptr, nullptr) &&
                  qso_store_worked(s, "K1ABC", "20M", "FT8", 0, "", nullptr),
              "away from an activation a station worked from home is worked");
        CHECK(!qso_store_worked(s, "K1ABC", "20M", "FT8", 0, "POTA:US-1234", nullptr), "from the park it is not yet");
        CHECK(qso_store_worked(s, "W9XY", "20M", "FT8", 0, "POTA:US-1234", &got_id) && got_id == one,
              "a station worked at this park is");
        CHECK(!qso_store_worked(s, "W9XY", "20M", "FT8", 0, "POTA:US-9999", nullptr) &&
                  !qso_store_worked(s, "W9XY", "20M", "FT8", 0, "POTA:US-123", nullptr),
              "at another park it is not, and a reference is matched whole");
        CHECK(qso_store_worked(s, "AA1A", "20M", "FT8", 0, "POTA:US-5678", &got_id) && got_id == two &&
                  qso_store_worked(s, "AA1A", "20M", "FT8", 0, "pota:us-1234", &got_id) && got_id == two,
              "a QSO logged at two parks counts at each, whatever the case");
        CHECK(qso_store_worked(s, "AA1A", "20M", "FT8", 0, "POTA:US-9999,SOTA:W7O/CC-001", &got_id) && got_id == summit,
              "any one of several references will do, a summit among them");
        CHECK(!qso_store_worked(s, "AA1A", "20M", "FT8", 0, "SOTA:US-1234", nullptr),
              "a park's reference is not looked for among summits");
        CHECK(qso_store_worked(s, "AA1A", "20M", "FT8", 0, "W7O/CC-001", &got_id) && got_id == summit &&
                  qso_store_worked(s, "W9XY", "20M", "FT8", 0, "US-1234", nullptr),
              "with no program named, a / makes it a summit");
        QsoStoreFilter at;
        at.date = 20261119;
        at.my_refs = "POTA:US-1234";
        CHECK(qso_store_select(s, at, 0, ids, 8, &matched) == 2 && ids[0] == one && ids[1] == two,
              "today's QSOs at a park, for its progress count");
        at.my_refs = "POTA:US-5678";
        CHECK(qso_store_select(s, at, 0, ids, 8, &matched) == 1 && ids[0] == two, "and at the other park of a 2-fer");
    }

    // Delete.
    end = rig.store.end;
    CHECK(qso_store_delete(&rig.store, d) == QsoStoreStatus::OK && rig.store.end == end + kQsoStoreEntryOverhead,
          "delete is one small entry");
    CHECK(!qso_store_get(rig.store, d, &got) && qso_store_delete(&rig.store, d) == QsoStoreStatus::NOT_FOUND &&
          patch(&rig.store, d, "<comment:1>x") == QsoStoreStatus::NOT_FOUND,
          "a deleted record is gone");
    CHECK(qso_store_worked(rig.store, "K1ABC", "20M", "FT8", 0, nullptr, &got_id) && got_id == a,
          "worked falls back to the record that remains");
    CHECK(!qso_store_find_qrz(rig.store, 55, &got_id), "and its QRZ ID names nothing");
    CHECK(add(&rig.store, "<call:4>AA1A", 0, &got_id) == QsoStoreStatus::OK && got_id == d + 1,
          "an ID is never given out twice");
}

static void test_import_and_meta()
{
    Rig rig;
    Disk disk;
    uint32_t own = 0, id = 0;
    std::string got;
    qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
    add(&rig.store, "<call:5>K1ABC<band:3>20M<mode:3>FT8", 0, &own);
    patch(&rig.store, own, "<app_qrzlog_logid:3>100");

    const char* theirs = "<call:4>W9XY<band:3>15M<mode:2>CW<app_qrzlog_logid:3>200<dxcc:3>291";
    CHECK(qso_store_import(&rig.store, theirs, strlen(theirs), &id) == QsoStoreStatus::OK && id == own + 1,
          "a record QRZ holds and we do not is added");
    CHECK(qso_store_entry(rig.store, id)->flags == kQsoStoreForeign, "as foreign");
    const size_t end = rig.store.end;
    CHECK(qso_store_import(&rig.store, theirs, strlen(theirs), &id) == QsoStoreStatus::UNCHANGED &&
          rig.store.end == end && id == own + 1,
          "sent again, nothing is written");
    const char* confirmed = "<app_qrzlog_logid:3>200<dxcc:3>291<app_qrzlog_status:1>C";
    CHECK(qso_store_import(&rig.store, confirmed, strlen(confirmed), &id) == QsoStoreStatus::OK &&
          qso_store_get(rig.store, id, &got) && as_map(got).at("app_qrzlog_status") == "C" &&
          as_map(got).at("call") == "W9XY",
          "what changed at QRZ is set on it");
    const char* ours = "<app_qrzlog_logid:3>100<dxcc:3>291<state:2>MA";
    CHECK(qso_store_import(&rig.store, ours, strlen(ours), &id) == QsoStoreStatus::OK && id == own &&
          qso_store_entry(rig.store, own)->flags == 0,
          "QRZ's fields for a record of ours land on ours, which stays ours");
    CHECK(qso_store_import(&rig.store, "<call:4>W9XY", 12, &id) == QsoStoreStatus::BAD_FIELDS,
          "an import needs QRZ's ID");

    CHECK(!qso_store_meta_get(rig.store, "qrz_modsince", &got), "no value yet");
    CHECK(qso_store_meta_set(&rig.store, "qrz_modsince", "2026-11-18") == QsoStoreStatus::OK, "set");
    CHECK(qso_store_meta_set(&rig.store, "qrz_modsince", "2026-11-19") == QsoStoreStatus::OK &&
          qso_store_meta_get(rig.store, "qrz_modsince", &got) && got == "2026-11-19",
          "the newest value wins");
    CHECK(qso_store_meta_set(&rig.store, "qrz_modsince", "2026-11-19") == QsoStoreStatus::UNCHANGED,
          "the same value writes nothing");
    CHECK(qso_store_meta_set(&rig.store, "a=b", "x") == QsoStoreStatus::BAD_FIELDS &&
          qso_store_meta_set(&rig.store, "", "x") == QsoStoreStatus::BAD_FIELDS,
          "a key is lower-case letters, digits and _");
    CHECK(qso_store_meta_set(&rig.store, "empty", "") == QsoStoreStatus::OK &&
          qso_store_meta_get(rig.store, "empty", &got) && got.empty(),
          "an empty value is a value");
    for (int i = 0; i < (int)kQsoStoreMetaKeys - 2; ++i) {
        CHECK(qso_store_meta_set(&rig.store, ("k" + std::to_string(i)).c_str(), "v") == QsoStoreStatus::OK,
              "key %d", i);
    }
    CHECK(qso_store_meta_set(&rig.store, "one_more", "v") == QsoStoreStatus::FULL, "more keys than the store holds");
}

// A journal with every kind of entry, records patched several times, a deleted one, an edited call.
static void build_journal(Rig* rig, Disk* disk)
{
    QsoStore* s = &rig->store;
    qso_store_create(s, rig->buffers(), 0xFEEDu, disk_append, disk);
    add(s, "<call:5>K1ABC<gridsquare:4>FN42<mode:3>FT8<qso_date:8>20261101<time_on:6>120000<band:3>20M<rst_sent:3>-10");
    add(s, "<call:4>W9XY<mode:4>MFSK<submode:3>FT4<qso_date:8>20261101<time_on:6>120100<band:3>40M"
           "<comment:10>caf\xC3\xA9 \xD5 5W");  // a comment holding the byte an entry starts with
    patch(s, 1, "<app_qrzlog_logid:4>1001<qrzcom_qso_upload_status:1>Y");
    qso_store_meta_set(s, "qrz_modsince", "2026-11-01");
    add(s, "<call:6>PJ4/K1<mode:3>FT8<qso_date:8>20261102<time_on:6>000500<band:3>20M");
    patch(s, 1, "<dxcc:3>291<state:2>MA<comment:5>first");
    patch(s, 2, "<app_qrzlog_logid:4>1002");
    add(s, "<call:5>JA1XX<mode:2>CW<band:3>15M<qso_date:8>20200704<app_qrzlog_logid:3>900<dxcc:3>339",
        kQsoStoreForeign);
    qso_store_delete(s, 3);
    patch(s, 1, "<comment:0><state:2>CT");
    qso_store_meta_set(s, "qrz_modsince", "2026-11-02");
    patch(s, 4, "<app_qrzlog_status:1>C");
    add(s, "<call:5>K1ABC<mode:3>FT8<qso_date:8>20261119<time_on:6>215400<band:3>20M");
    patch(s, 2, "<call:5>W9XYZ<comment:0>");
    qso_store_meta_set(s, "sync_note", "ok");
    add(s, "<call:4>VK2Z<mode:3>FT8<qso_date:8>20261119<time_on:6>215500<band:3>10M");
    patch(s, 6, "<gridsquare:4>QF56");
    patch(s, 5, "<qrzcom_qso_upload_status:1>Y");
}

// Each entry of the feed is lost in turn, and the journal is cut at every byte.
static void test_cut_and_damage()
{
    Rig rig;
    Disk disk;
    build_journal(&rig, &disk);
    const std::vector<Change> changes = read_feed(rig.store);
    CHECK(changes.size() == 18, "the journal holds %u entries", (unsigned)changes.size());
    verify(rig.store, replay(changes), "as built");
    CHECK(!replay(changes).recs.count(3) && replay(changes).recs.at(1).fields.at("state") == "CT",
          "the replay is the journal's meaning");

    // Reloaded whole, it answers as before and carries on.
    {
        Rig again;
        Disk copy = disk;
        const QsoStoreLoad load = again.load(&copy);
        CHECK(load.status == QsoStoreStatus::OK && load.valid_bytes == disk.bytes.size() && load.damaged_bytes == 0 &&
              load.ignored == 0,
              "a whole journal loads whole");
        verify(again.store, replay(changes), "reloaded");
        uint32_t id = 0;
        CHECK(add(&again.store, "<call:4>ZL1A", 0, &id) == QsoStoreStatus::OK && id == 7,
              "the next ID carries on from the journal");
    }

    // Cut at every length: what was wholly written is all there, and nothing else.
    for (size_t cut = 0; cut <= disk.bytes.size(); ++cut) {
        Rig again;
        Disk part;
        part.bytes.assign(disk.bytes.begin(), disk.bytes.begin() + cut);
        const QsoStoreLoad load = again.load(&part);
        if (cut < kQsoStoreHeaderBytes) {
            CHECK(load.status == QsoStoreStatus::BAD_HEADER, "cut at %u: not a journal", (unsigned)cut);
            continue;
        }
        std::set<size_t> lost;
        size_t whole = kQsoStoreHeaderBytes;
        for (size_t i = 0; i < changes.size(); ++i) {
            if (changes[i].at + changes[i].size <= cut) whole = changes[i].at + changes[i].size;
            else lost.insert(i);
        }
        char what[40];
        snprintf(what, sizeof what, "cut at %u", (unsigned)cut);
        CHECK(load.status == QsoStoreStatus::OK && load.valid_bytes == whole && load.damaged_bytes == 0,
              "%s: the journal ends at %u, after the last whole entry (%u)", what, (unsigned)load.valid_bytes,
              (unsigned)whole);
        verify(again.store, replay(changes, lost), what);

        // The caller cuts the file there; the next record goes on the end and survives a reload.
        part.bytes.resize(load.valid_bytes);
        uint32_t id = 0;
        CHECK(add(&again.store, "<call:4>ZL1A<band:3>20M<mode:3>FT8", 0, &id) == QsoStoreStatus::OK,
              "%s: logging carries on", what);
        Rig third;
        CHECK(third.load(&part).status == QsoStoreStatus::OK &&
              qso_store_worked(third.store, "ZL1A", "20M", "FT8", 0, nullptr, nullptr),
              "%s: and what was logged then is kept", what);
    }

    // One entry damaged: it alone is lost, with whatever depended on it.
    for (size_t k = 0; k < changes.size(); ++k) {
        Rig again;
        Disk hurt = disk;
        hurt.bytes[changes[k].at + changes[k].size - 1] ^= 0x40;  // in its CRC
        const QsoStoreLoad load = again.load(&hurt);
        const bool last = k + 1 == changes.size();
        char what[40];
        snprintf(what, sizeof what, "entry %u damaged", (unsigned)k);
        CHECK(load.status == QsoStoreStatus::OK, "%s: loads", what);
        CHECK(load.damaged_bytes == (last ? 0 : changes[k].size),
              "%s: %u bytes passed over", what, (unsigned)load.damaged_bytes);
        CHECK(load.valid_bytes == (last ? changes[k].at : disk.bytes.size()),
              "%s: journal ends at %u", what, (unsigned)load.valid_bytes);
        verify(again.store, replay(changes, {k}), what);
        // The feed of the damaged journal is the same changes without the lost one.
        const std::vector<Change> after = read_feed(again.store);
        CHECK(after.size() == changes.size() - 1, "%s: the feed passes over it", what);
        for (size_t i = 0; i < after.size(); ++i) {
            const Change& want = changes[i < k ? i : i + 1];
            CHECK(after[i].at == want.at && after[i].fields == want.fields && after[i].id == want.id,
                  "%s: feed entry %u", what, (unsigned)i);
        }
    }
}

// Bytes flipped at random, a few at a time, 4000 times: a load never fails, and loses exactly the entries
// that were hit. The previous-entry word is outside an entry's CRC, so a hit there loses nothing.
static void test_random_damage()
{
    Rig rig;
    Disk disk;
    build_journal(&rig, &disk);
    const std::vector<Change> changes = read_feed(rig.store);
    uint32_t seed = 12345;
    auto next = [&seed]() { return (seed = seed * 1664525u + 1013904223u) >> 8; };
    for (int round = 0; round < 4000; ++round) {
        Rig again;
        Disk hurt = disk;
        std::set<size_t> lost;
        const int hits = 1 + (int)(next() % 4);
        for (int h = 0; h < hits; ++h) {
            const size_t at = kQsoStoreHeaderBytes + next() % (disk.bytes.size() - kQsoStoreHeaderBytes);
            hurt.bytes[at] ^= (uint8_t)(1u << (next() % 8));
            for (size_t i = 0; i < changes.size(); ++i) {
                const size_t in = at - changes[i].at;
                if (at >= changes[i].at && in < changes[i].size && !(in >= 8 && in < 12)) lost.insert(i);
            }
        }
        for (size_t i = 0; i < changes.size(); ++i) {  // the same byte hit twice is no hit
            if (lost.count(i) && memcmp(&hurt.bytes[changes[i].at], &disk.bytes[changes[i].at], 8) == 0 &&
                memcmp(&hurt.bytes[changes[i].at + 12], &disk.bytes[changes[i].at + 12], changes[i].size - 12) == 0) {
                lost.erase(i);
            }
        }
        const QsoStoreLoad load = again.load(&hurt);
        char what[40];
        snprintf(what, sizeof what, "random damage %d", round);
        CHECK(load.status == QsoStoreStatus::OK, "%s: loads", what);
        verify(again.store, replay(changes, lost), what);
        if (g_fails) return;
    }
}

static void test_feed_cursor()
{
    Rig rig;
    Disk disk;
    build_journal(&rig, &disk);
    const std::vector<Change> changes = read_feed(rig.store);
    CHECK(changes[0].kind == QsoStoreChangeKind::ADDED && changes[0].id == 1 &&
          changes[2].kind == QsoStoreChangeKind::FIELDS_SET && changes[3].kind == QsoStoreChangeKind::META &&
          changes[8].kind == QsoStoreChangeKind::DELETED && changes[8].id == 3,
          "the feed names each kind of change");
    CHECK(changes[7].flags == kQsoStoreForeign && changes[0].flags == 0, "and whose a new record is");

    // A copy that is caught up hears only what came after.
    size_t cursor = rig.store.end;
    QsoStoreChange c;
    CHECK(qso_store_feed(rig.store, &cursor, &c) == QsoStoreFeed::CAUGHT_UP, "caught up");
    patch(&rig.store, 5, "<app_qrzlog_logid:4>1005");
    CHECK(qso_store_feed(rig.store, &cursor, &c) == QsoStoreFeed::CHANGE && c.kind == QsoStoreChangeKind::FIELDS_SET &&
          c.id == 5 && std::string(c.fields, c.len) == "<app_qrzlog_logid:4>1005",
          "then the one change since");
    CHECK(qso_store_feed(rig.store, &cursor, &c) == QsoStoreFeed::CAUGHT_UP && cursor == rig.store.end,
          "and caught up again");

    // Every cursor the feed gave out is good; nothing else is.
    std::set<size_t> good = {0, rig.store.end};
    for (const Change& change : read_feed(rig.store)) good.insert(change.at);
    for (size_t at = 0; at <= rig.store.end + 3; ++at) {
        size_t try_at = at;
        const QsoStoreFeed got = qso_store_feed(rig.store, &try_at, &c);
        CHECK((got != QsoStoreFeed::BAD_CURSOR) == (good.count(at) != 0),
              "cursor %u is %s", (unsigned)at, good.count(at) ? "refused" : "taken");
        if (got == QsoStoreFeed::BAD_CURSOR) CHECK(try_at == at, "a refused cursor is left as it was");
    }
}

static void test_limits_and_failures()
{
    // The append function refusing: nothing changes, and the next write goes through.
    {
        Rig rig;
        Disk disk;
        uint32_t id = 0;
        std::string got;
        disk.fail = true;
        CHECK(qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk) == QsoStoreStatus::WRITE_FAILED,
              "a journal that cannot be started says so");
        disk.fail = false;
        qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
        add(&rig.store, "<call:5>K1ABC<band:3>20M<mode:3>FT8", 0, &id);
        const size_t end = rig.store.end;
        disk.fail = true;
        CHECK(add(&rig.store, "<call:4>W9XY<band:3>20M<mode:3>FT8") == QsoStoreStatus::WRITE_FAILED,
              "a QSO that was not saved is not reported saved");
        CHECK(patch(&rig.store, id, "<comment:2>hi") == QsoStoreStatus::WRITE_FAILED &&
              qso_store_delete(&rig.store, id) == QsoStoreStatus::WRITE_FAILED &&
              qso_store_meta_set(&rig.store, "k", "v") == QsoStoreStatus::WRITE_FAILED,
              "nor a change to one");
        CHECK(rig.store.end == end && rig.store.next_id == id + 1 &&
              !qso_store_worked(rig.store, "W9XY", nullptr, nullptr, 0, nullptr, nullptr) &&
              qso_store_get(rig.store, id, &got) && got == "<call:5>K1ABC<band:3>20M<mode:3>FT8" &&
              !qso_store_meta_get(rig.store, "k", &got),
              "the store is as it was");
        disk.fail = false;
        CHECK(add(&rig.store, "<call:4>W9XY<band:3>20M<mode:3>FT8", 0, &id) == QsoStoreStatus::OK && id == 2 &&
              disk.bytes.size() == rig.store.end,
              "and the next write goes through");
    }
    // The journal's buffer full, to the byte.
    {
        const char* record = "<call:5>K1ABC<band:3>20M<mode:3>FT8";
        const size_t one = kQsoStoreEntryOverhead + strlen(record);
        Rig rig(kQsoStoreHeaderBytes + 3 * one + kQsoStoreEntryOverhead, 16, 32);
        Disk disk;
        uint32_t id = 0;
        qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
        CHECK(add(&rig.store, record) == QsoStoreStatus::OK && add(&rig.store, record) == QsoStoreStatus::OK &&
              add(&rig.store, record, 0, &id) == QsoStoreStatus::OK,
              "three records fit");
        CHECK(add(&rig.store, record) == QsoStoreStatus::FULL && disk.bytes.size() == rig.store.end,
              "a fourth does not, and nothing is written");
        CHECK(qso_store_delete(&rig.store, id) == QsoStoreStatus::OK, "what does fit still goes in");
        CHECK(qso_store_delete(&rig.store, 1) == QsoStoreStatus::FULL, "until nothing does");
    }
    // The index full; and a journal with more records than the index it is loaded into.
    {
        Rig rig(4096, 2, 4);
        Disk disk;
        qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
        add(&rig.store, "<call:4>AA1A");
        add(&rig.store, "<call:4>AA1B");
        const size_t end = rig.store.end;
        CHECK(add(&rig.store, "<call:4>AA1C") == QsoStoreStatus::FULL && rig.store.end == end,
              "a record the index has no place for is refused");
        CHECK(patch(&rig.store, 1, "<comment:2>hi") == QsoStoreStatus::OK, "a change to one it holds is not");
        Rig small(4096, 1, 2);
        CHECK(small.load(&disk).status == QsoStoreStatus::FULL,
              "a journal too big for the index is refused, not loaded in part");
    }
    // Buffers the store cannot use, and files that are not a journal.
    {
        Rig rig;
        Disk disk;
        QsoStoreBuffers bad = rig.buffers();
        bad.call_slot_count = 300;
        CHECK(qso_store_create(&rig.store, bad, 1, disk_append, &disk) == QsoStoreStatus::BAD_BUFFERS,
              "the call table must be a power of two");
        bad = rig.buffers();
        bad.call_slot_count = 256;
        CHECK(qso_store_create(&rig.store, bad, 1, disk_append, &disk) == QsoStoreStatus::BAD_BUFFERS,
              "and twice the index");
        qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
        Disk other = disk;
        other.bytes[4] = 2;
        CHECK(rig.load(&other).status == QsoStoreStatus::BAD_HEADER, "another version's journal is not read");
        other = disk;
        other.bytes[9] ^= 1;
        CHECK(rig.load(&other).status == QsoStoreStatus::BAD_HEADER, "nor one whose header fails its CRC");
        other.bytes.assign(1000, 0xFF);
        CHECK(rig.load(&other).status == QsoStoreStatus::BAD_HEADER, "nor erased flash");
        other.bytes.clear();
        CHECK(rig.load(&other).status == QsoStoreStatus::BAD_HEADER, "nor an empty file, which is created instead");
    }
}

// Enough records to fill the call table's chains, and enough call edits to make it rebuild.
static void test_many_records()
{
    Rig rig(1024 * 1024, 3000, 8192);
    Disk disk;
    char fields[96];
    char call[24];
    qso_store_create(&rig.store, rig.buffers(), 1, disk_append, &disk);
    for (int i = 0; i < 3000; ++i) {
        snprintf(call, sizeof call, "K%dAB%c", 1000 + i % 700, 'A' + i % 3);
        snprintf(fields, sizeof fields, "<call:%u>%s<band:3>%s<mode:3>FT8", (unsigned)strlen(call), call,
                 i % 2 ? "20M" : "40M");
        CHECK(add(&rig.store, fields) == QsoStoreStatus::OK, "record %d", i);
    }
    for (int round = 0; round < 4; ++round) {  // 4 x 3000 edits against 8192 slots
        for (uint32_t id = 1; id <= 3000; ++id) {
            snprintf(call, sizeof call, "N%uR%d", (unsigned)id, round);
            snprintf(fields, sizeof fields, "<call:%u>%s", (unsigned)strlen(call), call);
            CHECK(patch(&rig.store, id, fields) == QsoStoreStatus::OK, "edit %u", (unsigned)id);
        }
    }
    uint32_t got = 0;
    CHECK(qso_store_worked(rig.store, "N1234R3", "20M", "FT8", 0, nullptr, &got) && got == 1234,
          "a record is found under its newest call");
    CHECK(!qso_store_worked(rig.store, "N1234R2", nullptr, nullptr, 0, nullptr, nullptr) &&
          !qso_store_worked(rig.store, "K1000ABA", nullptr, nullptr, 0, nullptr, nullptr),
          "and under none of its old ones");
    const std::vector<Change> changes = read_feed(rig.store);
    Rig again(1024 * 1024, 3000, 8192);
    CHECK(again.load(&disk).status == QsoStoreStatus::OK, "15,000 entries load");
    verify(again.store, replay(changes), "many records, reloaded");
}

int main()
{
    test_golden_bytes();
    test_fields();
    test_patch_delete_worked();
    test_import_and_meta();
    test_cut_and_damage();
    test_random_damage();
    test_feed_cursor();
    test_limits_and_failures();
    test_many_records();
    if (g_fails) {
        fprintf(stderr, "%d FAIL(s)\n", g_fails);
        return 1;
    }
    printf("PASS: qso store\n");
    return 0;
}
