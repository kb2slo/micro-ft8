// The web app as one file: the reader. See app_pack.h.

#include "app_pack.h"

#include <string.h>

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A NUL-terminated string inside a fixed field of `max` bytes, at least one character long.
static bool field_text(const uint8_t *field, size_t max)
{
    return field[0] != '\0' && memchr(field, '\0', max) != NULL;
}

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

static bool name_ok(const uint8_t *field)
{
    if (!field_text(field, APP_PACK_NAME_MAX) || field[0] == '.') {
        return false;
    }
    for (const char *p = (const char *)field; *p; ++p) {
        if (!name_char(*p)) {
            return false;
        }
    }
    return true;
}

// The build id goes into JSON as it is (/api/config), so: printable ASCII, no quote or backslash.
static bool build_ok(const uint8_t *field)
{
    if (!field_text(field, APP_PACK_BUILD_MAX)) {
        return false;
    }
    for (const uint8_t *p = field; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static const uint8_t *entry_at(const app_pack_t *pack, uint16_t i)
{
    return pack->data + APP_PACK_HEAD_LEN + (size_t)i * APP_PACK_ENTRY_LEN;
}

size_t app_pack_claimed_len(const uint8_t *head)
{
    if (memcmp(head, APP_PACK_MAGIC, APP_PACK_MAGIC_LEN) != 0 || get_u16(head + 8) != APP_PACK_FORMAT) {
        return 0;
    }
    return get_u32(head + 12);
}

const char *app_pack_open(const uint8_t *data, size_t len, app_pack_t *out)
{
    if (!data || len < APP_PACK_HEAD_LEN) {
        return "short";
    }
    if (memcmp(data, APP_PACK_MAGIC, APP_PACK_MAGIC_LEN) != 0) {
        return "magic";
    }
    if (get_u16(data + 8) != APP_PACK_FORMAT) {
        return "format";
    }
    const uint16_t count = get_u16(data + 10);
    const size_t   total = get_u32(data + 12);
    const size_t   index_end = APP_PACK_HEAD_LEN + (size_t)count * APP_PACK_ENTRY_LEN;
    if (total > len) {
        return "length";
    }
    if (!build_ok(data + 16)) {
        return "build";
    }
    if (count == 0 || index_end > total) {
        return "index";
    }
    app_pack_t pack = {.data = data, .len = total, .count = count};
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *e = entry_at(&pack, i);
        if (!name_ok(e)) {
            return "name";
        }
        const size_t offset = get_u32(e + APP_PACK_NAME_MAX);
        const size_t size = get_u32(e + APP_PACK_NAME_MAX + 4);
        // Subtractions only: offset + size cannot be trusted not to wrap.
        if (offset < index_end || offset > total || size > total - offset) {
            return "bounds";
        }
    }
    memcpy(pack.build, data + 16, APP_PACK_BUILD_MAX);
    *out = pack;
    return NULL;
}

bool app_pack_entry(const app_pack_t *pack, uint16_t i, const char **name, const uint8_t **bytes, size_t *size)
{
    if (!pack || !pack->data || i >= pack->count) {
        return false;
    }
    const uint8_t *e = entry_at(pack, i);
    if (name) {
        *name = (const char *)e;
    }
    if (bytes) {
        *bytes = pack->data + get_u32(e + APP_PACK_NAME_MAX);
    }
    if (size) {
        *size = get_u32(e + APP_PACK_NAME_MAX + 4);
    }
    return true;
}

bool app_pack_find(const app_pack_t *pack, const char *name, const uint8_t **bytes, size_t *size)
{
    if (!pack || !pack->data || !name) {
        return false;
    }
    for (uint16_t i = 0; i < pack->count; ++i) {
        if (strcmp((const char *)entry_at(pack, i), name) == 0) {
            return app_pack_entry(pack, i, NULL, bytes, size);
        }
    }
    return false;
}
