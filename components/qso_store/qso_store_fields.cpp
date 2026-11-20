// A QSO record's fields as text: ADIF's `<name:length>value`, as callers hand them in and as the journal
// keeps them.

#include <cstdio>

#include "qso_store_internal.h"

namespace qso_store_detail {

static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// One `<name:length>value`, or `<name:length:type>value`. Text between fields is passed over, as ADIF
// allows; `<eor>` or the end of the text ends the record.
FieldRead next_field(const char** cursor, const char* end, Field* out)
{
    const char* p = *cursor;
    while (p < end && *p != '<') ++p;
    if (p >= end) return FieldRead::END;
    out->begin = p;
    const char* name = ++p;
    while (p < end && is_name_char(*p)) ++p;
    out->name = name;
    out->name_len = (size_t)(p - name);
    if (p >= end || out->name_len == 0 || out->name_len > kMaxNameChars) return FieldRead::MALFORMED;
    if (*p == '>') {
        const bool eor = out->name_len == 3 && to_lower(name[0]) == 'e' && to_lower(name[1]) == 'o' &&
                         to_lower(name[2]) == 'r';
        return eor ? FieldRead::END : FieldRead::MALFORMED;
    }
    if (*p != ':') return FieldRead::MALFORMED;
    size_t len = 0;
    const char* digits = ++p;
    while (p < end && *p >= '0' && *p <= '9' && len <= kQsoStoreMaxFields) len = len * 10 + (size_t)(*p++ - '0');
    if (p == digits || len > kQsoStoreMaxFields) return FieldRead::MALFORMED;
    if (p < end && *p == ':') {  // a data type indicator, which the store does not keep
        ++p;
        while (p < end && is_name_char(*p)) ++p;
    }
    if (p >= end || *p != '>' || len > (size_t)(end - p - 1)) return FieldRead::MALFORMED;
    out->value = p + 1;
    out->value_len = len;
    out->end = out->value + len;
    *cursor = out->end;
    return FieldRead::FIELD;
}

// In fields the store wrote, where names are lower case.
bool find_field(const char* fields, size_t len, const char* name, size_t name_len, Field* out)
{
    const char* p = fields;
    while (next_field(&p, fields + len, out) == FieldRead::FIELD) {
        if (out->name_len == name_len && memcmp(out->name, name, name_len) == 0) return true;
    }
    return false;
}

bool find_field(const std::string& fields, const char* name, Field* out)
{
    return find_field(fields.data(), fields.size(), name, strlen(name), out);
}

void append_field(std::string* fields, const char* name, size_t name_len, const char* value, size_t value_len)
{
    char head[kMaxNameChars + 16];
    const int n = snprintf(head, sizeof head, "<%.*s:%u>", (int)name_len, name, (unsigned)value_len);
    fields->append(head, (size_t)n);
    fields->append(value, value_len);
}

// The caller's fields as the journal keeps them: names lower case, back to back, a name given twice kept
// once with its last value. Fields of no length stay only when `keep_empty` (in a patch they remove).
bool canonical_fields(const char* fields, size_t len, bool keep_empty, std::string* out)
{
    out->clear();
    const char* p = fields;
    Field f;
    Field old;
    char name[kMaxNameChars];
    for (;;) {
        switch (next_field(&p, fields + len, &f)) {
            case FieldRead::END:
                return true;
            case FieldRead::MALFORMED:
                return false;
            case FieldRead::FIELD:
                break;
        }
        for (size_t i = 0; i < f.name_len; ++i) name[i] = to_lower(f.name[i]);
        if (find_field(out->data(), out->size(), name, f.name_len, &old)) {
            out->erase((size_t)(old.begin - out->data()), (size_t)(old.end - old.begin));
        }
        if (f.value_len != 0 || keep_empty) append_field(out, name, f.name_len, f.value, f.value_len);
    }
}

// A value that is all digits, at most `max_digits` of them; 0 for anything else.
uint64_t digits_value(const char* value, size_t len, size_t max_digits)
{
    if (len == 0 || len > max_digits) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9') return 0;
        v = v * 10 + (uint64_t)(value[i] - '0');
    }
    return v;
}

}  // namespace qso_store_detail
