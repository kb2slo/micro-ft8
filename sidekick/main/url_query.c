#include "url_query.h"

#include <ctype.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
            continue;
        }
        if (*r == '%') {
            const int hi = hex_value(r[1]);
            const int lo = hi < 0 ? -1 : hex_value(r[2]);
            if (lo >= 0) {
                *w++ = (char)(hi * 16 + lo);
                r += 2;
                continue;
            }
        }
        *w++ = *r;
    }
    *w = '\0';
}

bool url_query_callsign(const char *raw, char *out, size_t out_cap, size_t max_len)
{
    if (!out || out_cap == 0) return false;
    out[0] = '\0';
    if (!raw) return false;
    const size_t raw_n = strlen(raw);
    if (raw_n >= out_cap * 3) return false;  // longer than any call, encoded
    char buf[3 * 32 + 1];
    if (raw_n >= sizeof(buf)) return false;
    memcpy(buf, raw, raw_n + 1);
    url_decode(buf);
    const size_t n = strlen(buf);
    if (n == 0 || n > max_len || n >= out_cap) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!isalnum((unsigned char)buf[i]) && buf[i] != '/') return false;
    }
    memcpy(out, buf, n + 1);
    return true;
}
