// The decisions of a firmware update. See fw_policy.h.

#include "fw_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool fw_url_split(const char *url, char *host, size_t host_len, uint16_t *port, const char **path)
{
    if (!fw_url_ok(url)) {
        return false;
    }
    const char *authority = url + 7;
    const char *slash = strchr(authority, '/');
    const char *colon = memchr(authority, ':', (size_t)(slash - authority));
    const char *host_end = colon ? colon : slash;
    const size_t n = (size_t)(host_end - authority);
    if (n == 0 || n >= host_len) {
        return false;
    }
    memcpy(host, authority, n);
    host[n] = '\0';
    *port = 80;
    if (colon) {
        unsigned long v = 0;
        const char *p = colon + 1;
        if (p == slash || slash - p > 5) {
            return false;
        }
        for (; p < slash; ++p) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            v = v * 10 + (unsigned long)(*p - '0');
        }
        if (v == 0 || v > 65535) {
            return false;
        }
        *port = (uint16_t)v;
    }
    *path = slash;
    return true;
}

static bool header_is(const char *line, size_t line_len, const char *name, const char **value)
{
    const size_t n = strlen(name);
    if (line_len <= n || line[n] != ':') {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    const char *v = line + n + 1;
    while (v < line + line_len && (*v == ' ' || *v == '\t')) {
        ++v;
    }
    *value = v;
    return true;
}

int fw_http_head_parse(const char *buf, size_t len, int *status, int64_t *content_len)
{
    // The head ends at the first empty line.
    size_t head = 0;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            head = i + 4;
            break;
        }
    }
    if (head == 0) {
        return 0;
    }
    // "HTTP/1.x NNN ..."
    if (head < 14 || memcmp(buf, "HTTP/1.", 7) != 0 || buf[8] != ' ') {
        return -1;
    }
    int code = 0;
    for (int i = 9; i < 12; ++i) {
        if (buf[i] < '0' || buf[i] > '9') {
            return -1;
        }
        code = code * 10 + (buf[i] - '0');
    }
    *status = code;
    *content_len = -1;

    const char *line = memchr(buf, '\n', head);
    while (line && (size_t)(line + 1 - buf) < head) {
        ++line;
        const char *eol = memchr(line, '\r', head - (size_t)(line - buf));
        if (!eol) {
            return -1;
        }
        const size_t line_len = (size_t)(eol - line);
        const char *value;
        if (header_is(line, line_len, "content-length", &value)) {
            int64_t v = 0;
            if (value == eol) {
                return -1;
            }
            for (const char *p = value; p < eol; ++p) {
                if (*p < '0' || *p > '9' || v > 100000000) {
                    return -1;
                }
                v = v * 10 + (*p - '0');
            }
            *content_len = v;
        } else if (header_is(line, line_len, "transfer-encoding", &value)) {
            return -1;
        }
        line = memchr(eol, '\n', head - (size_t)(eol - buf));
    }
    return (int)head;
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
    if (req->board != FW_BOARD_SIDEKICK) {
        return "unsupported";
    }
    return NULL;
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
