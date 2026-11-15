// The rules of a bench push. See app_push.h.

#include "app_push.h"

#include <string.h>

#include "app_pack.h"

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A pack of `pack_len` bytes fits a slot of `slot_bytes` behind the header's sector.
static bool pack_fits(uint32_t pack_len, uint32_t slot_bytes)
{
    return pack_len >= APP_PACK_HEAD_LEN && slot_bytes > APP_PUSH_PACK_AT && pack_len <= slot_bytes - APP_PUSH_PACK_AT;
}

void app_push_head_write(const app_push_head_t *head, uint8_t out[APP_PUSH_RECORD_LEN])
{
    memcpy(out, APP_PUSH_MAGIC, APP_PUSH_MAGIC_LEN);
    put_u32(out + 8, APP_PUSH_FORMAT);
    put_u32(out + 12, head->pack_len);
    memcpy(out + 16, head->image, 32);
    memcpy(out + 48, head->pack_sha256, 32);
}

const char *app_push_head_read(const uint8_t record[APP_PUSH_RECORD_LEN], const char *version,
                               const uint8_t running[32], uint32_t slot_bytes, app_push_head_t *out)
{
    if (memcmp(record, APP_PUSH_MAGIC, APP_PUSH_MAGIC_LEN) != 0) {
        return "none";
    }
    if (app_push_is_release(version)) {
        return "release";
    }
    if (get_u32(record + 8) != APP_PUSH_FORMAT) {
        return "format";
    }
    if (memcmp(record + 16, running, 32) != 0) {
        return "image";
    }
    const uint32_t pack_len = get_u32(record + 12);
    if (!pack_fits(pack_len, slot_bytes)) {
        return "size";
    }
    out->pack_len = pack_len;
    memcpy(out->image, record + 16, 32);
    memcpy(out->pack_sha256, record + 48, 32);
    return NULL;
}

// One run of decimal digits; NULL if there is none.
static const char *skip_number(const char *p)
{
    if (*p < '0' || *p > '9') {
        return NULL;
    }
    while (*p >= '0' && *p <= '9') {
        ++p;
    }
    return p;
}

bool app_push_is_release(const char *version)
{
    const char *p = version;
    for (int part = 0; p && part < 3; ++part) {
        p = skip_number(p);
        if (p && part < 2) {
            p = *p == '.' ? p + 1 : NULL;
        }
    }
    if (!p || *p != '+') {
        return false;
    }
    ++p;
    for (int i = 0; i < 7; ++i, ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
            return false;
        }
    }
    return *p == '\0';
}

const char *app_push_refusal(const app_push_gate_t *gate)
{
    if (app_push_is_release(gate->version)) {
        return "release";
    }
    if (gate->busy) {
        return "busy";
    }
    if (gate->pending_verify) {
        return "confirming";
    }
    if (!pack_fits(gate->pack_len, gate->slot_bytes)) {
        return "size";
    }
    return NULL;
}
