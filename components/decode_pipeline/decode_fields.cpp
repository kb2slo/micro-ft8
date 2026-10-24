#include <cstring>

#include "decode_fields.h"

static bool all_digits(const char* s, int len)
{
    for (int i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static bool all_alpha(const char* s, int len)
{
    for (int i = 0; i < len; ++i) {
        const char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

static void copy_field(char* field, const char* token)
{
    strncpy(field, token, RX_FIELD_MAX - 1);
    field[RX_FIELD_MAX - 1] = '\0';
}

// toks[first..ntoks) joined by spaces, as long as it fits.
static void join_rest(char* field, char* const* toks, int first, int ntoks)
{
    field[0] = '\0';
    for (int i = first; i < ntoks; ++i) {
        if (i > first) strncat(field, " ", RX_FIELD_MAX - strlen(field) - 1);
        strncat(field, toks[i], RX_FIELD_MAX - strlen(field) - 1);
    }
}

void decode_fields_fill(RxDecodeEntry* d)
{
    d->field1[0] = d->field2[0] = d->field3[0] = '\0';
    char tmp[RX_TEXT_MAX];
    strncpy(tmp, d->text, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';

    char* saveptr = nullptr;
    char* toks[8];
    int ntoks = 0;
    for (char* p = strtok_r(tmp, " ", &saveptr); p && ntoks < 8; p = strtok_r(nullptr, " ", &saveptr)) {
        toks[ntoks++] = p;
    }
    if (ntoks == 0) return;

    // CQ <modifier> CALL GRID
    if (strcmp(toks[0], "CQ") == 0 && ntoks >= 2) {
        const int len1 = (int)strlen(toks[1]);
        const bool modifier = (len1 <= 3 && all_digits(toks[1], len1)) ||
                              (len1 <= 4 && all_alpha(toks[1], len1));
        if (modifier) {
            copy_field(d->field1, toks[1]);
            if (ntoks > 2) copy_field(d->field2, toks[2]);
            if (ntoks > 3) join_rest(d->field3, toks, 3, ntoks);
            return;
        }
    }

    // Default: first 2 tokens + remainder
    copy_field(d->field1, toks[0]);
    if (ntoks > 1) copy_field(d->field2, toks[1]);
    if (ntoks > 2) join_rest(d->field3, toks, 2, ntoks);
}
