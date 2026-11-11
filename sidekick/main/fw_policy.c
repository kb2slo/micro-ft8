// The decisions of a firmware update. See fw_policy.h.

#include "fw_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "porta_proto.h"

const char *fw_board_name(fw_board_t board)
{
    return board == FW_BOARD_MAIN ? "main" : "sidekick";
}

static bool board_from_name(const char *name, fw_board_t *out)
{
    if (strcmp(name, "sidekick") == 0) {
        *out = FW_BOARD_SIDEKICK;
        return true;
    }
    if (strcmp(name, "main") == 0) {
        *out = FW_BOARD_MAIN;
        return true;
    }
    return false;
}

// Printable ASCII that needs no escaping inside a JSON string.
static bool plain_char(char c)
{
    return c > ' ' && c < 0x7F && c != '"' && c != '\\';
}

bool fw_url_ok(const char *url)
{
    if (!url) {
        return false;
    }
    const size_t n = strlen(url);
    if (n >= FW_URL_MAX || strncmp(url, "http://", 7) != 0) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!plain_char(url[i])) {
            return false;
        }
    }
    // A host, then a path with something in it.
    const char *slash = strchr(url + 7, '/');
    return slash != NULL && slash != url + 7 && slash[1] != '\0';
}

bool fw_version_ok(const char *version)
{
    if (!version) {
        return false;
    }
    const size_t n = strlen(version);
    if (n == 0 || n >= FW_VERSION_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!plain_char(version[i])) {
            return false;
        }
    }
    return true;
}

static bool sha256_hex_ok(const char *s)
{
    if (strlen(s) != FW_SHA256_HEX) {
        return false;
    }
    for (size_t i = 0; i < FW_SHA256_HEX; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool size_from_text(const char *s, uint32_t *out)
{
    if (*s < '1' || *s > '9' || strlen(s) > 9) {  // nine digits fit a uint32_t
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

enum { F_BOARD = 1, F_URL = 2, F_SIZE = 4, F_SHA256 = 8, F_VERSION = 16, F_ALL = 31 };

const char *fw_request_parse(const char *body, fw_request_t *out)
{
    if (!body || !out) {
        return "body";
    }
    memset(out, 0, sizeof(*out));
    unsigned seen = 0;
    const char *p = body;
    while (*p) {
        const char *eol = p + strcspn(p, "\r\n");
        char line[FW_URL_MAX + 16];
        const size_t len = (size_t)(eol - p);
        if (len >= sizeof(line)) {
            return "body";
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol + strspn(eol, "\r\n");
        if (len == 0) {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            return "body";
        }
        *eq = '\0';
        const char *value = eq + 1;
        unsigned bit;
        bool ok;
        const char *field;
        if (strcmp(line, "board") == 0) {
            bit = F_BOARD, field = "board";
            ok = board_from_name(value, &out->board);
        } else if (strcmp(line, "url") == 0) {
            bit = F_URL, field = "url";
            ok = fw_url_ok(value);
            if (ok) {
                snprintf(out->url, sizeof(out->url), "%s", value);
            }
        } else if (strcmp(line, "size") == 0) {
            bit = F_SIZE, field = "size";
            ok = size_from_text(value, &out->size);
        } else if (strcmp(line, "sha256") == 0) {
            bit = F_SHA256, field = "sha256";
            ok = sha256_hex_ok(value);
            if (ok) {
                snprintf(out->sha256_hex, sizeof(out->sha256_hex), "%s", value);
            }
        } else if (strcmp(line, "version") == 0) {
            bit = F_VERSION, field = "version";
            ok = fw_version_ok(value);
            if (ok) {
                snprintf(out->version, sizeof(out->version), "%s", value);
            }
        } else {
            return "body";
        }
        if (!ok || (seen & bit)) {
            return field;
        }
        seen |= bit;
    }
    if (seen != F_ALL) {
        return !(seen & F_BOARD) ? "board" : !(seen & F_URL) ? "url" : !(seen & F_SIZE) ? "size"
             : !(seen & F_SHA256) ? "sha256" : "version";
    }
    return NULL;
}

const char *fw_plan_parse(const char *body, fw_request_t out[FW_PLAN_MAX], size_t *n_out)
{
    *n_out = 0;
    if (!body || strlen(body) >= FW_BODY_MAX) {
        return "body";
    }
    static const char SEP[] = "next";
    char        block[FW_BODY_MAX];
    const char *p = body;
    for (;;) {
        // This block ends at a line that reads `next`, or at the end of the body.
        const char *end = p;
        const char *after = NULL;
        while (*end) {
            const char *eol = end + strcspn(end, "\r\n");
            if ((size_t)(eol - end) == sizeof(SEP) - 1 && memcmp(end, SEP, sizeof(SEP) - 1) == 0) {
                after = eol + strspn(eol, "\r\n");
                break;
            }
            end = eol + strspn(eol, "\r\n");
        }
        if (*n_out == FW_PLAN_MAX) {
            return "plan";
        }
        memcpy(block, p, (size_t)(end - p));
        block[end - p] = '\0';
        const char *bad = fw_request_parse(block, &out[*n_out]);
        if (bad) {
            return bad;
        }
        for (size_t i = 0; i < *n_out; ++i) {
            // Each board once, and nothing after the sidekick's own image.
            if (out[i].board == out[*n_out].board || out[i].board == FW_BOARD_SIDEKICK) {
                return "plan";
            }
        }
        ++*n_out;
        if (!after) {
            return NULL;
        }
        p = after;
    }
}

const char *fw_start_refusal(const fw_request_t *req, const fw_gate_t *gate)
{
    if (gate->busy) {
        return "busy";
    }
    // The spare slot holds the image to go back to until this one is kept.
    if (gate->pending_verify) {
        return "confirming";
    }
    if (!gate->online) {
        return "offline";
    }
    if (req->size > gate->slot_bytes) {
        return "size";
    }
    if (req->board == FW_BOARD_MAIN && !gate->host_linked) {
        return "no_host";
    }
    return NULL;
}

bool fw_sha256_bytes(const char *hex, uint8_t out[32])
{
    if (!hex || !sha256_hex_ok(hex)) {
        return false;
    }
    for (int i = 0; i < 32; ++i) {
        unsigned v = 0;
        for (int j = 0; j < 2; ++j) {
            const char c = hex[2 * i + j];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

bool fw_image_version(const uint8_t *head, size_t len, char version_out[FW_VERSION_MAX])
{
    // esp_image_header_t (24 bytes, magic 0xE9), esp_image_segment_header_t (8),
    // then esp_app_desc_t: magic 0xABCD5432 little-endian, and version[32] at +16.
    if (!head || len < FW_IMAGE_HEAD || head[0] != 0xE9 || head[32] != 0x32 || head[33] != 0x54 ||
        head[34] != 0xCD || head[35] != 0xAB) {
        return false;
    }
    char version[FW_VERSION_MAX + 1];
    memcpy(version, &head[48], FW_VERSION_MAX);
    version[FW_VERSION_MAX] = '\0';
    if (!fw_version_ok(version)) {
        return false;
    }
    memcpy(version_out, version, strlen(version) + 1);  // fw_version_ok(): it fits
    return true;
}

const char *fw_transfer_fault(uint8_t status)
{
    switch (status) {
        case PORTA_FW_OK:
        case PORTA_FW_OFFSET:  // go on from the offset it wants
        case PORTA_FW_SHORT:   // likewise
            return NULL;
        case PORTA_FW_NOT_UPDATE_MODE: return "host_mode";
        case PORTA_FW_NO_SLOT:         return "no_slot";
        case PORTA_FW_TOO_BIG:         return "size";
        case PORTA_FW_NO_TRANSFER:     return "host_restarted";
        case PORTA_FW_FLASH:           return "host_flash";
        case PORTA_FW_HASH:            return "hash";
        case PORTA_FW_IMAGE:           return "image";
        case PORTA_FW_VERSION:         return "version";
        default:                       return "host";
    }
}

const char *fw_transfer_run(const fw_transfer_io_t *io, void *ctx, const porta_fw_begin_t *begin, uint8_t *buf,
                            int *detail, uint32_t *resumed_from)
{
    *resumed_from = 0;
    porta_fw_reply_t r = {0, 0};
    bool             answered = false;
    for (int i = 0; i < FW_BEGIN_TRIES && !answered; ++i) {
        answered = io->begin(ctx, begin, &r);
    }
    *detail = 0;
    if (!answered) {
        return "link";
    }
    const char *fault = fw_transfer_fault(r.status);
    if (fault || r.offset > begin->size) {
        *detail = r.status;
        return fault ? fault : "host";
    }
    uint32_t offset = r.offset;
    *resumed_from = offset;
    for (;;) {
        while (offset < begin->size) {
            if (io->cancelled(ctx)) {
                io->end(ctx, true, &r);
                return "cancelled";
            }
            const size_t n = begin->size - offset < PORTA_FW_CHUNK_MAX ? begin->size - offset : PORTA_FW_CHUNK_MAX;
            *detail = (int)offset;
            if (!io->read(ctx, offset, buf, n)) {
                return "flash";
            }
            answered = false;
            for (int i = 0; i < FW_CHUNK_TRIES && !answered; ++i) {
                answered = io->chunk(ctx, offset, buf, n, &r);
            }
            if (!answered) {
                return "link";
            }
            fault = fw_transfer_fault(r.status);
            // An answer that moves us backwards is the main board having lost bytes it acknowledged.
            if (fault || r.offset > begin->size || (r.status == PORTA_FW_OK && r.offset != offset + n)) {
                *detail = r.status;
                return fault ? fault : "host";
            }
            offset = r.offset;
            io->progress(ctx, offset);
        }
        // The main board installs on this. A second END after a lost answer is answered the same.
        answered = false;
        for (int i = 0; i < FW_END_TRIES && !answered; ++i) {
            answered = io->end(ctx, false, &r);
        }
        *detail = (int)offset;
        if (!answered) {
            return "link";
        }
        if (r.status == PORTA_FW_SHORT && r.offset < begin->size) {
            offset = r.offset;  // it is missing the tail: send it
            continue;
        }
        fault = fw_transfer_fault(r.status);
        if (fault || r.status != PORTA_FW_OK) {
            *detail = r.status;
            return fault ? fault : "host";
        }
        return NULL;
    }
}

fw_host_result_t fw_host_result(uint32_t since_end_ms, bool hello_seen, uint32_t hello_age_ms,
                                const char *hello_version, const char *target, bool on_trial)
{
    // Only a HELLO sent after the old image had time to restart counts.
    const bool fresh = hello_seen && since_end_ms >= FW_HOST_SETTLE_MS &&
                       hello_age_ms < since_end_ms - FW_HOST_SETTLE_MS;
    if (fresh && strcmp(hello_version, target) != 0) {
        return FW_HOST_ROLLED_BACK;
    }
    if (fresh && !on_trial) {
        return FW_HOST_UPDATED;
    }
    return since_end_ms >= FW_HOST_CONFIRM_MS ? FW_HOST_SILENT : FW_HOST_WAIT;
}

bool fw_record_format(const fw_record_t *rec, char *out, size_t out_len)
{
    if (!fw_version_ok(rec->target) || !fw_version_ok(rec->from)) {
        return false;
    }
    const int n = snprintf(out, out_len, "%s\t%s\t%s", fw_board_name(rec->board), rec->target, rec->from);
    return n > 0 && (size_t)n < out_len;
}

bool fw_record_parse(const char *text, fw_record_t *out)
{
    if (!text || strlen(text) >= FW_RECORD_MAX) {
        return false;
    }
    char buf[FW_RECORD_MAX];
    snprintf(buf, sizeof(buf), "%s", text);
    char *t1 = strchr(buf, '\t');
    char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (!t2 || strchr(t2 + 1, '\t')) {
        return false;
    }
    *t1 = '\0';
    *t2 = '\0';
    memset(out, 0, sizeof(*out));
    if (!board_from_name(buf, &out->board) || !fw_version_ok(t1 + 1) || !fw_version_ok(t2 + 1)) {
        return false;
    }
    snprintf(out->target, sizeof(out->target), "%s", t1 + 1);
    snprintf(out->from, sizeof(out->from), "%s", t2 + 1);
    return true;
}

fw_last_t fw_last_result(const fw_record_t *rec, const char *running_version, bool pending_verify)
{
    if (strcmp(rec->target, running_version) != 0) {
        return FW_LAST_ROLLED_BACK;
    }
    return pending_verify ? FW_LAST_CONFIRMING : FW_LAST_OK;
}

const char *fw_last_name(fw_last_t result)
{
    switch (result) {
        case FW_LAST_OK:          return "ok";
        case FW_LAST_ROLLED_BACK: return "rolled_back";
        default:                  return "confirming";
    }
}

fw_confirm_t fw_confirm_decide(uint32_t uptime_s, uint32_t beacons_sent, bool reachable)
{
    if (beacons_sent >= FW_CONFIRM_BEACONS && reachable) {
        return FW_CONFIRM_KEEP;
    }
    return uptime_s >= FW_CONFIRM_TIMEOUT_S ? FW_CONFIRM_ROLL_BACK : FW_CONFIRM_WAIT;
}
