#pragma once

// fw_policy.h -- the decisions of a firmware update, with no ESP-IDF in them
// (docs/architecture/sidekick.md (Firmware updates)). fw_update.c does the
// download, the flash writes and the restart, and asks here what a request
// says, whether an update may start, what the last one came to, and whether a
// freshly installed image has proved itself. Host-tested
// (host_test_fw_policy).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_URL_MAX     192
#define FW_VERSION_MAX 32  // esp_app_desc_t's version[32], NUL included
#define FW_SHA256_HEX  64

typedef enum {
    FW_BOARD_SIDEKICK = 0,
    FW_BOARD_MAIN     = 1,
} fw_board_t;

// "sidekick" or "main": the names the HTTP API uses.
const char *fw_board_name(fw_board_t board);

// What the web app read from the release manifest over HTTPS and hands over
// behind the pairing token.
typedef struct {
    fw_board_t board;
    char       url[FW_URL_MAX];
    uint32_t   size;
    char       sha256_hex[FW_SHA256_HEX + 1];
    char       version[FW_VERSION_MAX];
} fw_request_t;

// True for a plain-HTTP URL we will fetch: "http://host/path", no spaces,
// quotes, backslashes or control characters, short enough to store. HTTPS is
// refused because the sidekick has no TLS; the hash is what is trusted.
bool fw_url_ok(const char *url);

// Splits a URL fw_url_ok() accepts into its host, its port (80 unless the URL
// names one) and its path, which points into `url`. False if the host does
// not fit or the port is not a number from 1 to 65535.
bool fw_url_split(const char *url, char *host, size_t host_len, uint16_t *port, const char **path);

// Reads the head of an HTTP response as it arrives. Returns the number of
// bytes the head takes, so the body starts there, with the status code and
// the Content-Length (-1 if the reply has none) filled in; 0 when the head is
// not all in `buf` yet; -1 for anything that is not a response this client
// can read, a chunked body included (it asks with HTTP/1.0, which has none).
int fw_http_head_parse(const char *buf, size_t len, int *status, int64_t *content_len);

// True for a version string an app image can carry and JSON can quote as it is.
bool fw_version_ok(const char *version);

// Parses POST /api/fw/start's body: one `key=value` per line, the keys board,
// url, size, sha256 and version, each exactly once, in any order. Returns
// NULL on success, else the name of the field that is missing, repeated or
// malformed ("body" when a line is not key=value or the key is unknown).
const char *fw_request_parse(const char *body, fw_request_t *out);

typedef struct {
    bool     online;          // joined to a WiFi network (not its own AP)
    bool     busy;            // an update is already running
    bool     pending_verify;  // the running image has not been confirmed yet
    uint32_t slot_bytes;      // size of the spare OTA slot the image is staged in
} fw_gate_t;

// NULL if the update may start, else why not, as the code the API reports:
// "busy", "confirming", "offline", "size" or "unsupported".
const char *fw_start_refusal(const fw_request_t *req, const fw_gate_t *gate);

// The record kept across the restart that ends an update, so the next boot
// can say what came of it: "board\ttarget\tfrom".
#define FW_RECORD_MAX (8 + 1 + FW_VERSION_MAX + 1 + FW_VERSION_MAX)

typedef struct {
    fw_board_t board;
    char       target[FW_VERSION_MAX];  // the version the update installed
    char       from[FW_VERSION_MAX];    // the version that was running
} fw_record_t;

// Writes the record into out; false if it does not fit.
bool fw_record_format(const fw_record_t *rec, char *out, size_t out_len);
bool fw_record_parse(const char *text, fw_record_t *out);

typedef enum {
    FW_LAST_CONFIRMING = 0,  // the new image is running and has not proved itself yet
    FW_LAST_OK,              // the new image is running and was kept
    FW_LAST_ROLLED_BACK,     // something else is running: the new image did not stay
} fw_last_t;

// What came of the sidekick's own last update, from the record and what is running now.
fw_last_t fw_last_result(const fw_record_t *rec, const char *running_version, bool pending_verify);
const char *fw_last_name(fw_last_t result);  // "confirming", "ok", "rolled_back"

// An image installed over the air boots on trial: the bootloader goes back to
// the one before it at the next restart unless the image is marked valid.
// It is kept once it has done what the next update needs of it: sent its
// beacons to the main board, and either joined the WiFi network again or
// answered a client over HTTP (which is how it is reached on its own access
// point). Joining is enough because the phone that started the update may be
// locked, with nothing left polling. One that has done neither by the
// deadline restarts itself, which is the roll back.
#define FW_CONFIRM_BEACONS   3u
#define FW_CONFIRM_TIMEOUT_S 300u

typedef enum {
    FW_CONFIRM_WAIT = 0,
    FW_CONFIRM_KEEP,
    FW_CONFIRM_ROLL_BACK,
} fw_confirm_t;

fw_confirm_t fw_confirm_decide(uint32_t uptime_s, uint32_t beacons_sent, bool reachable);

#ifdef __cplusplus
}
#endif
