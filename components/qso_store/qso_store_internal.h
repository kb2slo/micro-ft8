#pragma once

// What the store's own source files share: the journal's bytes, a record's fields as text, and the index
// helpers the queries read through. Not for callers, whose header is include/qso_store.h.

#include <cstring>

#include "qso_store.h"

namespace qso_store_detail {

constexpr uint8_t kEntryMagic = 0xD5;
constexpr size_t kEntryHeaderBytes = 16;
constexpr size_t kPrevAt = 8;  // the previous-entry word, which the CRC leaves out
constexpr size_t kMaxNameChars = 40;  // an ADIF field's name

enum EntryKind : uint8_t { kPut = 1, kPatch = 2, kDelete = 3, kMeta = 4 };

struct JournalEntry {
    uint8_t kind;
    uint8_t flags;
    uint32_t id;
    uint32_t prev;
    const char* fields;
    size_t len;
    size_t size;  // header, fields and CRC
};

struct Field {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
    const char* begin;  // the '<'
    const char* end;    // one past the value
};

enum class FieldRead { FIELD, END, MALFORMED };

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
inline uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
inline void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
inline char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// The journal's bytes (qso_store_journal.cpp).
uint32_t crc32_add(uint32_t crc, const uint8_t* p, size_t n);
uint32_t entry_crc(const uint8_t* at, size_t len);
bool read_entry(const uint8_t* image, size_t end, size_t at, JournalEntry* out);
size_t next_entry(const uint8_t* image, size_t end, size_t from, JournalEntry* out);

// A record's fields as text (qso_store_fields.cpp).
FieldRead next_field(const char** cursor, const char* end, Field* out);
bool find_field(const char* fields, size_t len, const char* name, size_t name_len, Field* out);
bool find_field(const std::string& fields, const char* name, Field* out);
void append_field(std::string* fields, const char* name, size_t name_len, const char* value, size_t value_len);
bool canonical_fields(const char* fields, size_t len, bool keep_empty, std::string* out);
uint64_t digits_value(const char* value, size_t len, size_t max_digits);

// The index (qso_store.cpp).
void upper_name(const char* value, size_t len, char out[kQsoStoreNameChars + 1]);
uint8_t name_index(const QsoStoreNames& names, const char* upper);
uint32_t call_hash(const char* call);
QsoStoreEntry* find_entry(const QsoStore& store, uint32_t id);
bool current_fields(const QsoStore& store, const QsoStoreEntry& entry, std::string* out);
size_t meta_key_len(const char* fields, size_t len);
int meta_index(const QsoStore& store, const char* key, size_t key_len);

}  // namespace qso_store_detail
