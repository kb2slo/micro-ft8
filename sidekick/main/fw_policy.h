#pragma once

// fw_policy.h -- the decisions of a firmware update, with no ESP-IDF in them
// (docs/architecture/sidekick.md (Firmware updates)). fw_update.c does the
// download, the flash writes and the restart, and asks here what a request
// says, whether an update may start, what the last one came to, whether a
// freshly installed image has proved itself, and how a main board's image is
// sent to it. Host-tested (host_test_fw_policy).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "porta_proto.h"

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

// True for a version string an app image can carry and JSON can quote as it is.
bool fw_version_ok(const char *version);

// Parses one image's request: one `key=value` per line, the keys board, url,
// size, sha256 and version, each exactly once, in any order. Returns NULL on
// success, else the name of the field that is missing, repeated or malformed
// ("body" when a line is not key=value or the key is unknown).
const char *fw_request_parse(const char *body, fw_request_t *out);

// Parses POST /api/fw/start's body: one request, or FW_PLAN_MAX of them
// separated by a line reading `next`, carried out in that order with no page
// needed in between. Each board appears once, and the sidekick's own image is
// last, since installing it restarts the sidekick. Returns NULL with the
// requests in `out` and their count in `*n_out`, else the field at fault as
// fw_request_parse() names it, or "plan" for a plan that breaks those rules.
#define FW_PLAN_MAX  2
#define FW_BODY_MAX  (FW_PLAN_MAX * (FW_URL_MAX + 192))
const char *fw_plan_parse(const char *body, fw_request_t out[FW_PLAN_MAX], size_t *n_out);

typedef struct {
    bool     online;          // joined to a WiFi network (not its own AP)
    bool     busy;            // an update is already running
    bool     pending_verify;  // the running image has not been confirmed yet
    uint32_t slot_bytes;      // size of the spare OTA slot the image is staged in
    bool     host_linked;     // the main board's HELLO is arriving
} fw_gate_t;

// NULL if the update may start, else why not, as the code the API reports:
// "busy", "confirming", "offline", "size", or "no_host" (a main-board image
// with no main board answering, or one from before it could be updated).
const char *fw_start_refusal(const fw_request_t *req, const fw_gate_t *gate);

// 64 lowercase hex digits to 32 bytes; false for anything else.
bool fw_sha256_bytes(const char *hex, uint8_t out[32]);

// The version built into an ESP-IDF app image, from its first FW_IMAGE_HEAD
// bytes: the image header, the first segment's header, then the app
// description. False if those bytes are not the start of an app image. Used
// for a main board's image, which ESP-IDF's own check here would refuse as
// built for another chip; the main board checks the rest when it arrives.
#define FW_IMAGE_HEAD 288u
bool fw_image_version(const uint8_t *head, size_t len, char version_out[FW_VERSION_MAX]);

// What the main board's answer to a firmware message means for the transfer
// (porta_fw_status_t): NULL to go on from the offset it gave, else the code
// the update fails with: "radio" (a v16 main board, with one attached), "no_slot", "size", "host_restarted",
// "host_flash", "hash", "image", "version" or "host".
const char *fw_transfer_fault(uint8_t status);

// Sends a staged image to the main board, a message and its answer at a time
// (porta_proto.h, Firmware update). The main board says where to go on from
// in every answer, so a lost frame costs one resend, and a transfer it
// already holds part of goes on from there. Each `io` call that exchanges a
// message returns false for no answer, and is tried FW_BEGIN_TRIES,
// FW_CHUNK_TRIES or FW_END_TRIES times. Returns NULL once the main board has
// accepted the whole image (it then restarts into it), else the code the
// update fails with: fw_transfer_fault()'s, "link" (no answer), "flash" (the
// staged image could not be read) or "cancelled" (and the main board was told
// to forget the transfer). `*detail` gets the status or offset beside it, and
// `*resumed_from` the offset the main board asked to start at (0 for a new transfer).
// `buf` holds PORTA_FW_CHUNK_MAX bytes.
#define FW_BEGIN_TRIES 3
#define FW_CHUNK_TRIES 5
#define FW_END_TRIES   2

typedef struct {
    bool (*read)(void *ctx, uint32_t offset, uint8_t *out, size_t len);
    bool (*begin)(void *ctx, const porta_fw_begin_t *begin, porta_fw_reply_t *out);
    bool (*chunk)(void *ctx, uint32_t offset, const uint8_t *data, size_t len, porta_fw_reply_t *out);
    bool (*end)(void *ctx, bool abort, porta_fw_reply_t *out);
    bool (*cancelled)(void *ctx);
    void (*progress)(void *ctx, uint32_t offset);
} fw_transfer_io_t;

const char *fw_transfer_run(const fw_transfer_io_t *io, void *ctx, const porta_fw_begin_t *begin, uint8_t *buf,
                            int *detail, uint32_t *resumed_from);

// After a main board was told to install an image and restart, what its
// HELLO says. Its old image is still running for a moment after it answers,
// so a HELLO within FW_HOST_SETTLE_MS of that is not evidence either way.
#define FW_HOST_SETTLE_MS  4000u
#define FW_HOST_CONFIRM_MS 240000u

typedef enum {
    FW_HOST_WAIT = 0,
    FW_HOST_UPDATED,      // it reports the target version and has kept it
    FW_HOST_ROLLED_BACK,  // it restarted and reports another
    FW_HOST_SILENT,       // nothing by the deadline
} fw_host_result_t;

// `since_end_ms`: time since the main board accepted the image. `hello_age_ms`:
// how long ago its last HELLO came; `hello_seen` false if none ever has.
// `on_trial`: the main board has not said it kept the image it is running
// (it answers PORTA_FW_TRIAL, or does not answer). The new version reporting
// in is not enough: an image that cannot hear the sidekick says HELLO for two
// minutes and then goes back.
fw_host_result_t fw_host_result(uint32_t since_end_ms, bool hello_seen, uint32_t hello_age_ms,
                                const char *hello_version, const char *target, bool on_trial);

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
