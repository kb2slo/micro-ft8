#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Query and form values as a browser sends them: httpd_query_key_value() matches the key and leaves the
// value encoded, so `/` arrives as `%2F` from encodeURIComponent() and a space as `+`.

// Decodes `s` in place: `+` to a space, `%XX` to its byte. An escape that is not two hex digits is kept as
// it is, so a value with a stray `%` survives whole.
void url_decode(char *s);

// The callsign a `call=` value names, decoded into `out` (`out_cap` bytes): 1 to `max_len` letters,
// digits and `/`. False for anything else, `out` then empty.
bool url_query_callsign(const char *raw, char *out, size_t out_cap, size_t max_len);

#ifdef __cplusplus
}
#endif
