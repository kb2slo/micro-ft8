// The QSO store's reads: a record, worked before, a page of records, the feed (include/qso_store.h).

#include "qso_store_internal.h"

#include <algorithm>
#include <vector>

using namespace qso_store_detail;

bool qso_store_get(const QsoStore& store, uint32_t id, std::string* fields_out)
{
    const QsoStoreEntry* entry = find_entry(store, id);
    return entry != nullptr && current_fields(store, *entry, fields_out);
}

const QsoStoreEntry* qso_store_entry(const QsoStore& store, uint32_t id)
{
    const QsoStoreEntry* entry = find_entry(store, id);
    return entry != nullptr && entry->latest != 0 ? entry : nullptr;
}

bool qso_store_find_qrz(const QsoStore& store, uint64_t qrz_logid, uint32_t* id_out)
{
    if (qrz_logid == 0) return false;
    for (size_t i = 0; i < store.entries_used; ++i) {
        const QsoStoreEntry& entry = store.buffers.entries[i];
        if (entry.latest != 0 && entry.qrz_logid == qrz_logid) {
            *id_out = entry.id;
            return true;
        }
    }
    return false;
}

// Whether `list`, a record's comma list of references, holds `ref`, without regard to case.
static bool list_holds(const Field& list, const char* ref, size_t ref_len)
{
    const char* p = list.value;
    const char* end = list.value + list.value_len;
    while (p < end) {
        const char* item = p;
        while (p < end && *p != ',') ++p;
        const char* item_end = p;
        while (item < item_end && *item == ' ') ++item;
        while (item_end > item && item_end[-1] == ' ') --item_end;
        if ((size_t)(item_end - item) == ref_len) {
            size_t i = 0;
            while (i < ref_len && to_upper(item[i]) == to_upper(ref[i])) ++i;
            if (i == ref_len) return true;
        }
        if (p < end) ++p;
    }
    return false;
}

// Whether a record was logged at one of `my_refs` ("POTA:US-1234,SOTA:W7O/CC-001"): its MY_POTA_REF holds
// one of the parks or its MY_SOTA_REF one of the summits. Reads the record's fields, so it is asked last,
// of the few records everything else has let through.
static bool logged_at(const QsoStore& store, const QsoStoreEntry& entry, const char* my_refs)
{
    std::string fields;
    Field parks{};
    Field summits{};
    if (!current_fields(store, entry, &fields)) return false;
    const bool has_parks = find_field(fields, "my_pota_ref", &parks);
    const bool has_summits = find_field(fields, "my_sota_ref", &summits);
    for (const char* p = my_refs; *p != '\0';) {
        const char* item = p;
        while (*p != '\0' && *p != ',') ++p;
        const char* item_end = p;
        if (*p == ',') ++p;
        while (item < item_end && *item == ' ') ++item;
        while (item_end > item && item_end[-1] == ' ') --item_end;
        const char* colon = (const char*)memchr(item, ':', (size_t)(item_end - item));
        const char* ref = colon != nullptr ? colon + 1 : item;
        const size_t ref_len = (size_t)(item_end - ref);
        if (ref_len == 0) continue;
        bool summit = memchr(ref, '/', ref_len) != nullptr;
        if (colon != nullptr) summit = to_upper(item[0]) == 'S';  // "SOTA:", against "POTA:"
        if (summit ? has_summits && list_holds(summits, ref, ref_len) : has_parks && list_holds(parks, ref, ref_len)) {
            return true;
        }
    }
    return false;
}

bool qso_store_worked(const QsoStore& store, const char* call, const char* band, const char* mode,
                      uint32_t since_date, const char* my_refs, uint32_t* id_out)
{
    char upper[kQsoStoreNameChars + 1];
    uint8_t band_at = kQsoStoreNoName;
    uint8_t mode_at = kQsoStoreNoName;
    if (call == nullptr || store.buffers.call_slots == nullptr) return false;
    if (band != nullptr) {
        upper_name(band, strlen(band), upper);
        band_at = name_index(store.bands, upper);
        if (band_at == kQsoStoreNoName) return false;  // no record is on a band the store has never seen
    }
    if (mode != nullptr) {
        upper_name(mode, strlen(mode), upper);
        mode_at = name_index(store.modes, upper);
        if (mode_at == kQsoStoreNoName) return false;
    }
    const size_t mask = store.buffers.call_slot_count - 1;
    uint32_t best = 0;
    for (size_t at = call_hash(call) & mask; store.buffers.call_slots[at] != 0; at = (at + 1) & mask) {
        const QsoStoreEntry& entry = store.buffers.entries[store.buffers.call_slots[at] - 1];
        if (entry.latest == 0 || entry.id <= best || entry.date < since_date) continue;
        if ((band != nullptr && entry.band != band_at) || (mode != nullptr && entry.mode != mode_at)) continue;
        const char* a = entry.call;
        const char* b = call;
        while (*a != '\0' && *a == to_upper(*b)) {
            ++a;
            ++b;
        }
        if (*a != '\0' || *b != '\0') continue;
        if (my_refs != nullptr && my_refs[0] != '\0' && !logged_at(store, entry, my_refs)) continue;
        best = entry.id;
    }
    if (best != 0 && id_out != nullptr) *id_out = best;
    return best != 0;
}

static bool matches(const QsoStore& store, const QsoStoreEntry& entry, const QsoStoreFilter& filter)
{
    if (entry.latest == 0 || (filter.date != 0 && entry.date != filter.date)) return false;
    if (filter.unsent && (entry.qrz_logid != 0 || entry.uploaded)) return false;
    const bool foreign = (entry.flags & kQsoStoreForeign) != 0;
    switch (filter.whose) {
        case QsoStoreWhose::ANY:
            break;
        case QsoStoreWhose::OWN:
            if (foreign) return false;
            break;
        case QsoStoreWhose::FOREIGN:
            if (!foreign) return false;
            break;
    }
    if (filter.call != nullptr) {
        const char* a = entry.call;
        const char* b = filter.call;
        while (*a != '\0' && *a == to_upper(*b)) {
            ++a;
            ++b;
        }
        if (*a != '\0' || *b != '\0') return false;
    }
    const bool at_refs = filter.my_refs != nullptr && filter.my_refs[0] != '\0';
    return !at_refs || logged_at(store, entry, filter.my_refs);
}

// Each entry that may match, once: with a call, the call table's slots for it; else the index, newest ID
// first, since our own records were logged in date order and so mostly arrive in the order a page wants.
template <typename Fn>
static void each_candidate(const QsoStore& store, const QsoStoreFilter& filter, Fn fn)
{
    if (filter.call != nullptr) {
        if (store.buffers.call_slots == nullptr) return;
        const size_t mask = store.buffers.call_slot_count - 1;
        for (size_t at = call_hash(filter.call) & mask; store.buffers.call_slots[at] != 0; at = (at + 1) & mask) {
            fn(store.buffers.entries[store.buffers.call_slots[at] - 1]);
        }
        return;
    }
    for (size_t n = store.entries_used; n-- > 0;) fn(store.buffers.entries[n]);
}

size_t qso_store_select(const QsoStore& store, const QsoStoreFilter& filter, size_t skip, uint32_t* ids,
                        size_t take, size_t* matched_out)
{
    size_t matched = 0;
    size_t written = 0;
    for (size_t n = 0; n < store.entries_used; ++n) {
        const QsoStoreEntry& entry = store.buffers.entries[filter.newest_first ? store.entries_used - 1 - n : n];
        if (!matches(store, entry, filter)) continue;
        if (matched >= skip && written < take) ids[written++] = entry.id;
        ++matched;
    }
    if (matched_out != nullptr) *matched_out = matched;
    return written;
}

namespace {

// A record's place in date, time and ID order, as one number: larger is newer.
struct PageKey {
    uint64_t when;  // date and time, 202611251432
    uint32_t id;
};

bool newer(const PageKey& a, const PageKey& b) { return a.when != b.when ? a.when > b.when : a.id > b.id; }

PageKey key_of(uint32_t date, uint16_t time, uint32_t id) { return PageKey{(uint64_t)date * 10000u + time, id}; }

}  // namespace

size_t qso_store_page(const QsoStore& store, const QsoStoreFilter& filter, const QsoStoreCursor& before,
                      uint32_t* ids, size_t take, size_t* matched_out, QsoStoreSummary* summary_out)
{
    const bool from_newest = qso_store_cursor_start(before);
    const PageKey limit = key_of(before.date, before.time, before.id);
    std::vector<PageKey> kept;  // newest first, at most `take`
    kept.reserve(take);
    size_t matched = 0;
    // For the summary: the distinct dates, kept sorted, and a count for each band and mode seen.
    std::vector<uint32_t> dates;
    std::vector<QsoStoreSummary::Pair> pairs;
    uint32_t first = 0, last = 0;
    each_candidate(store, filter, [&](const QsoStoreEntry& entry) {
        if (!matches(store, entry, filter)) return;
        ++matched;
        if (summary_out != nullptr) {
            if (entry.date != 0) {
                if (first == 0 || entry.date < first) first = entry.date;
                if (entry.date > last) last = entry.date;
                auto d = std::lower_bound(dates.begin(), dates.end(), entry.date);
                if (d == dates.end() || *d != entry.date) dates.insert(d, entry.date);
            }
            auto p = std::find_if(pairs.begin(), pairs.end(), [&](const QsoStoreSummary::Pair& q) {
                return q.band == entry.band && q.mode == entry.mode;
            });
            if (p == pairs.end()) pairs.push_back({entry.band, entry.mode, 1});
            else ++p->count;
        }
        if (take == 0) return;
        const PageKey k = key_of(entry.date, entry.time, entry.id);
        if (!from_newest && !newer(limit, k)) return;
        if (kept.size() == take && !newer(k, kept.back())) return;
        auto at = std::upper_bound(kept.begin(), kept.end(), k, [](const PageKey& a, const PageKey& b) { return newer(a, b); });
        if (kept.size() == take) kept.pop_back();
        kept.insert(at, k);
    });
    for (size_t i = 0; i < kept.size(); ++i) ids[i] = kept[i].id;
    if (matched_out != nullptr) *matched_out = matched;
    if (summary_out != nullptr) {
        QsoStoreSummary& s = *summary_out;
        s = QsoStoreSummary{};
        s.count = matched;
        s.first_date = first;
        s.last_date = last;
        s.days = dates.size();
        std::sort(pairs.begin(), pairs.end(), [](const QsoStoreSummary::Pair& a, const QsoStoreSummary::Pair& b) {
            if (a.count != b.count) return a.count > b.count;
            return a.band != b.band ? a.band < b.band : a.mode < b.mode;
        });
        s.pairs_total = pairs.size();
        s.pair_count = std::min(pairs.size(), kQsoStoreSummaryPairs);
        for (size_t i = 0; i < s.pair_count; ++i) s.pairs[i] = pairs[i];
    }
    return kept.size();
}

const char* qso_store_band_name(const QsoStore& store, uint8_t band)
{
    return band < store.bands.used ? store.bands.name[band] : "";
}

const char* qso_store_mode_name(const QsoStore& store, uint8_t mode)
{
    return mode < store.modes.used ? store.modes.name[mode] : "";
}

bool qso_store_meta_get(const QsoStore& store, const char* key, std::string* value_out)
{
    const size_t key_len = strlen(key);
    const int i = meta_index(store, key, key_len);
    if (i < 0) return false;
    const uint8_t* h = store.buffers.image + store.meta_at[i];
    value_out->assign((const char*)h + kEntryHeaderBytes + key_len + 1, rd16(h + 2) - key_len - 1);
    return true;
}

QsoStoreFeed qso_store_feed(const QsoStore& store, size_t* cursor_io, QsoStoreChange* change_out)
{
    JournalEntry j;
    size_t at = *cursor_io;
    if (at == 0) {
        at = next_entry(store.buffers.image, store.end, kQsoStoreHeaderBytes, &j);
    } else if (at != store.end && !read_entry(store.buffers.image, store.end, at, &j)) {
        return QsoStoreFeed::BAD_CURSOR;
    }
    if (at == store.end) {
        *cursor_io = store.end;
        return QsoStoreFeed::CAUGHT_UP;
    }
    change_out->flags = j.flags;
    change_out->id = j.id;
    change_out->fields = j.fields;
    change_out->len = j.len;
    switch (j.kind) {
        case kPut:
            change_out->kind = QsoStoreChangeKind::ADDED;
            break;
        case kPatch:
            change_out->kind = QsoStoreChangeKind::FIELDS_SET;
            break;
        case kDelete:
            change_out->kind = QsoStoreChangeKind::DELETED;
            break;
        default:  // read_entry() passes only the four kinds
            change_out->kind = QsoStoreChangeKind::META;
            break;
    }
    // The cursor handed back is the next readable entry, so it never points into damage.
    JournalEntry after;
    *cursor_io = next_entry(store.buffers.image, store.end, at + j.size, &after);
    return QsoStoreFeed::CHANGE;
}
