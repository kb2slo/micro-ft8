#pragma once

// app_push.h -- the rules of a bench push (docs/architecture/sidekick.md (The bench push)): a web
// app sent from a workstation (webapp/tools/install_app.py) and served in the image's place, on
// the image it was pushed onto and no other. app_serve.c does the flash and the HTTP; this is what
// a push's header says, whether it names the running image, and whether one may be written at
// all. No ESP-IDF in it. Host-tested (host_test_app_push).
//
// A push lives in the spare firmware slot: a header in the slot's first flash sector, then the
// pack (app_pack.h) from the second. The header is written last and erased first, so a slot holds
// a whole push or none.
//
//   0   8   "MFT8PUSH"
//   8   4   format, APP_PUSH_FORMAT
//   12  4   the pack's length
//   16  32  SHA-256 of the ELF of the image it was pushed onto (esp_app_desc_t.app_elf_sha256)
//   48  32  SHA-256 of the pack

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PUSH_MAGIC      "MFT8PUSH"
#define APP_PUSH_MAGIC_LEN  8u
#define APP_PUSH_FORMAT     1u
#define APP_PUSH_RECORD_LEN 80u    // the header's bytes
#define APP_PUSH_PACK_AT    4096u  // where the pack starts: the slot's second flash sector

typedef struct {
    uint32_t pack_len;
    uint8_t  image[32];
    uint8_t  pack_sha256[32];
} app_push_head_t;

void app_push_head_write(const app_push_head_t *head, uint8_t out[APP_PUSH_RECORD_LEN]);

// Reads a slot's first APP_PUSH_RECORD_LEN bytes, for the image whose version is `version` and
// whose ELF hash is `running`. NULL, with `out` filled, when they are the header of a push onto
// that image, with a pack that fits a slot of `slot_bytes`. Else why it is not served, for a log
// line: "none" (no push there: the usual state, a firmware image or erased flash), "release" (a
// release serves no push, whatever its slot holds: the slot has other writers than the push, and
// the rule is that a release serves only its own app, not only that it accepts no other),
// "format", "image" (pushed onto another image: the pin that makes any firmware change return a
// station to its image's app) or "size".
const char *app_push_head_read(const uint8_t record[APP_PUSH_RECORD_LEN], const char *version,
                               const uint8_t running[32], uint32_t slot_bytes, app_push_head_t *out);

// True for a release's version, "0.1.0+0c5ccec" (tools/build_version.py). A dev build's
// ("b1322+0c5ccec") and a bench build's (its bare SHA, "-dirty" or not) are not.
bool app_push_is_release(const char *version);

typedef struct {
    bool        busy;            // a firmware update is running, and staging its image in the slot
    bool        pending_verify;  // the running image is on trial: the slot holds the one to go back to
    const char *version;         // the running image's
    uint32_t    slot_bytes;      // the spare slot's size
    uint32_t    pack_len;        // the request's Content-Length
} app_push_gate_t;

// NULL if a push may be written, else why not, as the code the API reports: "busy", "confirming",
// "release" (a release serves only the app it was built with, so it stays the code its dev build
// was) or "size".
const char *app_push_refusal(const app_push_gate_t *gate);

#ifdef __cplusplus
}
#endif
