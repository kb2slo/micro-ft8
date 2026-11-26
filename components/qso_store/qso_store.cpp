// The QSO store: the index over the journal, and everything that writes to either (include/qso_store.h).

#include "qso_store_internal.h"

using namespace qso_store_detail;

static constexpr uint16_t kVersion = 1;

enum class Tag { CALL, BAND, MODE, SUBMODE, QSO_DATE, TIME_ON, QRZ_LOGID, QRZ_UPLOADED, OTHER };

static Tag tag_of(const Field& f)
{
    static const struct {
        const char* name;
        Tag tag;
    } kTags[] = {
        {"call", Tag::CALL},         {"band", Tag::BAND},       {"mode", Tag::MODE},
        {"submode", Tag::SUBMODE},   {"qso_date", Tag::QSO_DATE}, {"time_on", Tag::TIME_ON},
        {"app_qrzlog_logid", Tag::QRZ_LOGID}, {"qrzcom_qso_upload_status", Tag::QRZ_UPLOADED},
    };
    for (const auto& t : kTags) {
        if (strlen(t.name) == f.name_len && memcmp(t.name, f.name, f.name_len) == 0) return t.tag;
    }
    return Tag::OTHER;
}

static bool call_ok(const Field& f)
{
    if (f.value_len == 0 || f.value_len > kQsoStoreCallChars) return false;
    for (size_t i = 0; i < f.value_len; ++i) {
        if (f.value[i] <= ' ' || f.value[i] > '~') return false;
    }
    return true;
}

void qso_store_detail::upper_name(const char* value, size_t len, char out[kQsoStoreNameChars + 1])
{
    if (len > kQsoStoreNameChars) len = kQsoStoreNameChars;
    for (size_t i = 0; i < len; ++i) out[i] = to_upper(value[i]);
    out[len] = '\0';
}

uint8_t qso_store_detail::name_index(const QsoStoreNames& names, const char* upper)
{
    for (uint8_t i = 0; i < names.used; ++i) {
        if (strcmp(names.name[i], upper) == 0) return i;
    }
    return kQsoStoreNoName;
}

static uint8_t intern_name(QsoStoreNames* names, const char* value, size_t len)
{
    char upper[kQsoStoreNameChars + 1];
    if (len == 0) return kQsoStoreNoName;
    upper_name(value, len, upper);
    const uint8_t at = name_index(*names, upper);
    if (at != kQsoStoreNoName || names->used == kQsoStoreNames) return at;
    memcpy(names->name[names->used], upper, sizeof upper);
    return names->used++;
}

uint32_t qso_store_detail::call_hash(const char* call)
{
    uint32_t h = 2166136261u;  // FNV-1a
    for (; *call; ++call) h = (h ^ (uint8_t)to_upper(*call)) * 16777619u;
    return h;
}

static void slot_insert(QsoStore* store, size_t entry_index)
{
    const size_t mask = store->buffers.call_slot_count - 1;
    size_t at = call_hash(store->buffers.entries[entry_index].call) & mask;
    while (store->buffers.call_slots[at] != 0) at = (at + 1) & mask;
    store->buffers.call_slots[at] = (uint32_t)(entry_index + 1);
    ++store->slots_used;
}

// Files an entry under its call. A slot is never taken back, so a record whose call was edited, or which
// was deleted, leaves one behind that lookups pass over; when those have filled three quarters of the
// table it is rebuilt from the records that remain, which are at most half of it.
static void note_call(QsoStore* store, size_t entry_index)
{
    if ((store->slots_used + 1) * 4 <= store->buffers.call_slot_count * 3) {
        slot_insert(store, entry_index);
        return;
    }
    memset(store->buffers.call_slots, 0, store->buffers.call_slot_count * sizeof(uint32_t));
    store->slots_used = 0;
    for (size_t i = 0; i < store->entries_used; ++i) {
        if (store->buffers.entries[i].latest != 0) slot_insert(store, i);
    }
}

QsoStoreEntry* qso_store_detail::find_entry(const QsoStore& store, uint32_t id)
{
    size_t lo = 0;
    size_t hi = store.entries_used;
    while (lo < hi) {  // IDs only ever go up, so the index is in ID order
        const size_t mid = lo + (hi - lo) / 2;
        if (store.buffers.entries[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    if (lo == store.entries_used || store.buffers.entries[lo].id != id) return nullptr;
    return &store.buffers.entries[lo];
}

// A record's fields as they now stand: its entries from the newest back to the one that added it, each
// field taken from the newest entry that names it, then the removed ones dropped.
bool qso_store_detail::current_fields(const QsoStore& store, const QsoStoreEntry& entry, std::string* out)
{
    out->clear();
    if (entry.latest == 0) return false;
    // A record never changed is its one entry, whose fields qso_store_add() wrote as canonical_fields()
    // gives them: no name twice and none empty, so they are already what the walk below would build.
    const uint8_t* newest = store.buffers.image + entry.latest;
    if (newest[1] == kPut) {
        out->assign((const char*)newest + kEntryHeaderBytes, rd16(newest + 2));
        return true;
    }
    Field f;
    Field seen;
    size_t at = entry.latest;
    for (;;) {
        const uint8_t* h = store.buffers.image + at;
        const char* p = (const char*)h + kEntryHeaderBytes;
        const char* end = p + rd16(h + 2);
        while (next_field(&p, end, &f) == FieldRead::FIELD) {
            if (!find_field(out->data(), out->size(), f.name, f.name_len, &seen)) {
                append_field(out, f.name, f.name_len, f.value, f.value_len);
            }
        }
        if (h[1] == kPut) break;
        const size_t prev = rd32(h + kPrevAt);
        if (prev == 0 || prev >= at) return false;  // loading leaves no such chain
        at = prev;
    }
    for (;;) {
        const char* p = out->data();
        bool removed = false;
        while (next_field(&p, out->data() + out->size(), &f) == FieldRead::FIELD) {
            if (f.value_len == 0) {
                out->erase((size_t)(f.begin - out->data()), (size_t)(f.end - f.begin));
                removed = true;
                break;
            }
        }
        if (!removed) return true;
    }
}

// Brings an index entry up to date with the fields of one journal entry, the record's newest.
static void apply_fields(QsoStore* store, QsoStoreEntry* entry, const JournalEntry& j)
{
    const char* p = j.fields;
    Field f;
    bool mode_named = false;
    while (next_field(&p, j.fields + j.len, &f) == FieldRead::FIELD) {
        switch (tag_of(f)) {
            case Tag::CALL:
                if (call_ok(f)) {
                    for (size_t i = 0; i < f.value_len; ++i) entry->call[i] = to_upper(f.value[i]);
                    entry->call[f.value_len] = '\0';
                }
                break;
            case Tag::BAND:
                entry->band = intern_name(&store->bands, f.value, f.value_len);
                break;
            case Tag::MODE:
            case Tag::SUBMODE:
                mode_named = true;
                break;
            case Tag::QSO_DATE:
                entry->date = f.value_len == 8 ? (uint32_t)digits_value(f.value, 8, 8) : 0;
                break;
            case Tag::TIME_ON:
                entry->time = f.value_len >= 4 ? (uint16_t)digits_value(f.value, 4, 4) : 0;
                break;
            case Tag::QRZ_LOGID:
                entry->qrz_logid = digits_value(f.value, f.value_len, 19);
                break;
            case Tag::QRZ_UPLOADED:
                // QRZ refuses a QSO it already holds and names no ID for it, so the ID alone cannot say "sent".
                entry->uploaded = f.value_len == 1 && to_upper(f.value[0]) == 'Y';
                break;
            case Tag::OTHER:
                break;
        }
    }
    if (!mode_named) return;
    Field mode;
    entry->mode = kQsoStoreNoName;
    // MODE and SUBMODE decide the mode together. A record's first entry holds both, so they are read from it;
    // a change may name one, so then the whole record is read.
    if (j.kind == kPut) {
        if (find_field(j.fields, j.len, "submode", 7, &mode) || find_field(j.fields, j.len, "mode", 4, &mode)) {
            entry->mode = intern_name(&store->modes, mode.value, mode.value_len);
        }
        return;
    }
    std::string now;
    if (!current_fields(*store, *entry, &now)) return;
    if (find_field(now, "submode", &mode) || find_field(now, "mode", &mode)) {
        entry->mode = intern_name(&store->modes, mode.value, mode.value_len);
    }
}

size_t qso_store_detail::meta_key_len(const char* fields, size_t len)
{
    size_t n = 0;
    while (n < len && fields[n] != '=') ++n;
    return n;
}

int qso_store_detail::meta_index(const QsoStore& store, const char* key, size_t key_len)
{
    for (int i = 0; i < store.meta_used; ++i) {
        const uint8_t* h = store.buffers.image + store.meta_at[i];
        const char* fields = (const char*)h + kEntryHeaderBytes;
        if (meta_key_len(fields, rd16(h + 2)) == key_len && memcmp(fields, key, key_len) == 0) return i;
    }
    return -1;
}

// Takes one journal entry into the index. False when it names a record the journal does not hold, which
// only damage before it can cause.
static bool index_entry(QsoStore* store, size_t at, const JournalEntry& j)
{
    QsoStoreEntry* entry = nullptr;
    switch (j.kind) {
        case kPut: {
            if (j.id < store->next_id || store->entries_used == store->buffers.entry_count) return false;
            store->next_id = j.id + 1;  // never given out again, even if the entry is not kept
            Field call;
            if (!find_field(j.fields, j.len, "call", 4, &call) || !call_ok(call)) return false;
            entry = &store->buffers.entries[store->entries_used];
            *entry = QsoStoreEntry{};
            entry->id = j.id;
            entry->latest = (uint32_t)at;
            entry->flags = j.flags;
            entry->band = kQsoStoreNoName;
            entry->mode = kQsoStoreNoName;
            ++store->entries_used;
            apply_fields(store, entry, j);
            note_call(store, store->entries_used - 1);
            return true;
        }
        case kPatch: {
            entry = find_entry(*store, j.id);
            if (entry == nullptr || entry->latest == 0) return false;
            wr32(store->buffers.image + at + kPrevAt, entry->latest);
            entry->latest = (uint32_t)at;
            char call[kQsoStoreCallChars + 1];
            memcpy(call, entry->call, sizeof call);
            apply_fields(store, entry, j);
            if (strcmp(call, entry->call) != 0) note_call(store, (size_t)(entry - store->buffers.entries));
            return true;
        }
        case kDelete:
            entry = find_entry(*store, j.id);
            if (entry == nullptr || entry->latest == 0) return false;
            entry->latest = 0;
            return true;
        case kMeta: {
            const size_t key_len = meta_key_len(j.fields, j.len);
            int i = meta_index(*store, j.fields, key_len);
            if (i < 0) {
                if (key_len == j.len || store->meta_used == kQsoStoreMetaKeys) return false;
                i = store->meta_used++;
            }
            store->meta_at[i] = (uint32_t)at;
            return true;
        }
        default:
            return false;
    }
}

// Writes one entry at the journal's end, makes it durable, then takes it into the index.
static QsoStoreStatus append_entry(QsoStore* store, uint8_t kind, uint8_t flags, uint32_t id, uint32_t prev,
                            const std::string& fields)
{
    if (fields.size() > kQsoStoreMaxFields) return QsoStoreStatus::TOO_LONG;
    const size_t size = kQsoStoreEntryOverhead + fields.size();
    if (store->buffers.image_bytes - store->end < size) return QsoStoreStatus::FULL;
    uint8_t* h = store->buffers.image + store->end;
    memset(h, 0, kEntryHeaderBytes);
    h[0] = kEntryMagic;
    h[1] = kind;
    wr16(h + 2, (uint16_t)fields.size());
    wr32(h + 4, id);
    wr32(h + kPrevAt, prev);
    h[12] = flags;
    memcpy(h + kEntryHeaderBytes, fields.data(), fields.size());
    wr32(h + kEntryHeaderBytes + fields.size(), entry_crc(h, fields.size()));
    if (store->append == nullptr || !store->append(store->append_ctx, h, size)) return QsoStoreStatus::WRITE_FAILED;
    JournalEntry j;
    const size_t at = store->end;
    store->end += size;
    read_entry(store->buffers.image, store->end, at, &j);
    index_entry(store, at, j);
    return QsoStoreStatus::OK;
}

static bool buffers_ok(const QsoStoreBuffers& b)
{
    return b.image != nullptr && b.entries != nullptr && b.call_slots != nullptr &&
           b.image_bytes >= kQsoStoreHeaderBytes && b.image_bytes <= UINT32_MAX && b.entry_count >= 1 &&
           b.entry_count <= UINT32_MAX / 2 && (b.call_slot_count & (b.call_slot_count - 1)) == 0 &&
           b.call_slot_count >= 2 * b.entry_count;
}

static void start(QsoStore* store, const QsoStoreBuffers& buffers, QsoStoreAppendFn append, void* append_ctx)
{
    *store = QsoStore{};
    store->buffers = buffers;
    store->append = append;
    store->append_ctx = append_ctx;
    store->next_id = 1;
    memset(buffers.call_slots, 0, buffers.call_slot_count * sizeof(uint32_t));
}

QsoStoreStatus qso_store_create(QsoStore* store, const QsoStoreBuffers& buffers, uint64_t store_id,
                                QsoStoreAppendFn append, void* append_ctx)
{
    if (!buffers_ok(buffers)) return QsoStoreStatus::BAD_BUFFERS;
    start(store, buffers, append, append_ctx);
    uint8_t* h = buffers.image;
    memcpy(h, "MFQS", 4);
    wr16(h + 4, kVersion);
    wr16(h + 6, (uint16_t)kQsoStoreHeaderBytes);
    wr64(h + 8, store_id);
    wr32(h + 16, 0);
    wr32(h + 20, crc32_add(0xFFFFFFFFu, h, 20) ^ 0xFFFFFFFFu);
    if (append == nullptr || !append(append_ctx, h, kQsoStoreHeaderBytes)) return QsoStoreStatus::WRITE_FAILED;
    store->store_id = store_id;
    store->end = kQsoStoreHeaderBytes;
    return QsoStoreStatus::OK;
}

QsoStoreLoad qso_store_load(QsoStore* store, const QsoStoreBuffers& buffers, size_t bytes,
                            QsoStoreAppendFn append, void* append_ctx)
{
    QsoStoreLoad out{};
    out.status = QsoStoreStatus::BAD_BUFFERS;
    if (!buffers_ok(buffers) || bytes > buffers.image_bytes) return out;
    start(store, buffers, append, append_ctx);
    const uint8_t* h = buffers.image;
    out.status = QsoStoreStatus::BAD_HEADER;
    if (bytes < kQsoStoreHeaderBytes || memcmp(h, "MFQS", 4) != 0 || rd16(h + 4) != kVersion ||
        rd16(h + 6) != kQsoStoreHeaderBytes || rd32(h + 20) != (crc32_add(0xFFFFFFFFu, h, 20) ^ 0xFFFFFFFFu)) {
        return out;
    }
    store->store_id = rd64(h + 8);
    store->generation = rd32(h + 16);
    store->end = kQsoStoreHeaderBytes;
    out.status = QsoStoreStatus::OK;
    JournalEntry j;
    for (;;) {
        const size_t at = next_entry(buffers.image, bytes, store->end, &j);
        if (at == bytes) break;  // nothing readable is left: what remains was a write cut short
        if (j.kind == kPut && j.id >= store->next_id && store->entries_used == buffers.entry_count) {
            out.status = QsoStoreStatus::FULL;
            break;
        }
        out.damaged_bytes += at - store->end;
        if (!index_entry(store, at, j)) ++out.ignored;
        store->end = at + j.size;
    }
    out.valid_bytes = store->end;
    return out;
}

QsoStoreStatus qso_store_add(QsoStore* store, const char* fields, size_t len, uint8_t flags, uint32_t* id_out)
{
    std::string canonical;
    Field call;
    if (!canonical_fields(fields, len, false, &canonical)) return QsoStoreStatus::BAD_FIELDS;
    if (!find_field(canonical, "call", &call) || !call_ok(call)) return QsoStoreStatus::BAD_FIELDS;
    if (store->entries_used == store->buffers.entry_count) return QsoStoreStatus::FULL;
    const uint32_t id = store->next_id;
    const QsoStoreStatus status = append_entry(store, kPut, flags, id, 0, canonical);
    if (status == QsoStoreStatus::OK && id_out != nullptr) *id_out = id;
    return status;
}

QsoStoreStatus qso_store_patch(QsoStore* store, uint32_t id, const char* fields, size_t len)
{
    const QsoStoreEntry* entry = find_entry(*store, id);
    if (entry == nullptr || entry->latest == 0) return QsoStoreStatus::NOT_FOUND;
    std::string asked;
    std::string now;
    std::string change;
    if (!canonical_fields(fields, len, true, &asked) || !current_fields(*store, *entry, &now)) {
        return QsoStoreStatus::BAD_FIELDS;
    }
    const char* p = asked.data();
    Field f;
    Field held;
    while (next_field(&p, asked.data() + asked.size(), &f) == FieldRead::FIELD) {
        const bool has = find_field(now.data(), now.size(), f.name, f.name_len, &held);
        const bool same = f.value_len == 0 ? !has
                                           : has && held.value_len == f.value_len &&
                                                 memcmp(held.value, f.value, f.value_len) == 0;
        if (same) continue;
        if (tag_of(f) == Tag::CALL && !call_ok(f)) return QsoStoreStatus::BAD_FIELDS;
        change.append(f.begin, (size_t)(f.end - f.begin));
    }
    if (change.empty()) return QsoStoreStatus::UNCHANGED;
    return append_entry(store, kPatch, 0, id, entry->latest, change);
}

QsoStoreStatus qso_store_delete(QsoStore* store, uint32_t id)
{
    const QsoStoreEntry* entry = find_entry(*store, id);
    if (entry == nullptr || entry->latest == 0) return QsoStoreStatus::NOT_FOUND;
    return append_entry(store, kDelete, 0, id, entry->latest, std::string());
}

QsoStoreStatus qso_store_import(QsoStore* store, const char* fields, size_t len, uint32_t* id_out)
{
    std::string canonical;
    Field logid;
    if (!canonical_fields(fields, len, true, &canonical) || !find_field(canonical, "app_qrzlog_logid", &logid)) {
        return QsoStoreStatus::BAD_FIELDS;
    }
    const uint64_t qrz_logid = digits_value(logid.value, logid.value_len, 19);
    if (qrz_logid == 0) return QsoStoreStatus::BAD_FIELDS;
    uint32_t id = 0;
    if (!qso_store_find_qrz(*store, qrz_logid, &id)) {
        return qso_store_add(store, fields, len, kQsoStoreForeign, id_out);
    }
    if (id_out != nullptr) *id_out = id;
    return qso_store_patch(store, id, fields, len);
}

QsoStoreStatus qso_store_meta_set(QsoStore* store, const char* key, const char* value)
{
    const size_t key_len = strlen(key);
    if (key_len == 0 || key_len > kQsoStoreMetaKeyChars) return QsoStoreStatus::BAD_FIELDS;
    for (size_t i = 0; i < key_len; ++i) {
        if (!((key[i] >= 'a' && key[i] <= 'z') || (key[i] >= '0' && key[i] <= '9') || key[i] == '_')) {
            return QsoStoreStatus::BAD_FIELDS;
        }
    }
    std::string held;
    if (qso_store_meta_get(*store, key, &held)) {
        if (held == value) return QsoStoreStatus::UNCHANGED;
    } else if (store->meta_used == kQsoStoreMetaKeys) {
        return QsoStoreStatus::FULL;
    }
    return append_entry(store, kMeta, 0, 0, 0, std::string(key) + "=" + value);
}
