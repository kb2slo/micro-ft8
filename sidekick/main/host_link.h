#pragma once

// ============================================================================
// host_link.h — what the host is doing, received over PORTA and served to a
// browser (I28a, docs/architecture/sidekick.md (Port A)).
//
// Read-only on purpose. This is the first slice that runs the whole stack for
// real -- host -> PORTA -> sidekick -> HTTP -> phone -- and it does so without
// needing the control direction, the pairing token, the app partition or the
// signing chain. None of those can block a working end-to-end path, and a
// viewer cannot key a transmitter.
//
// It also gives the link a free correctness oracle: the same lines appear on
// the main board's own screen (`P` then `.`), so if the two disagree the fault is
// between them.
// ============================================================================

#include "esp_http_server.h"

// Starts the PORTA receive task. Independent of WiFi -- events are printed on
// this device's USB-C whether or not anything is serving them, because that
// console is the point when the main board's own USB-C is busy hosting the radio.
void host_link_start(void);

// Adds the viewer and its polling endpoint to an already-started server.
void host_link_register_uris(httpd_handle_t server);

// The sidekick's own line in the event ring, as a log event like the main board's: /api/events and the app's
// log show it. Truncated to one event's text. Takes the ring's lock, so not from inside it.
void host_link_log(const char *text);

// The link's health for the console heartbeat (main.c's alive line): internal heap free, lowest and largest
// block, the ring's next sequence number and gap count, and Port A's frames and CRC errors. Out of band, so
// it keeps reporting while the web server is stalled (row dlu3).
void host_link_health(char *out, size_t cap);

// A request or a Port A wait slower than this is logged (row dlu3, docs/ROADMAP.md): the server runs one
// request at a time, so a slow one holds every other.
#define HOST_LINK_SLOW_MS 500

// Asks the main board to reset its Station settings (and, when `logs`, delete its QSO logs).
// Blocks for the answer. true = the host did it; false = it did not, with the reason in `why`
// ("host link busy", "no reply from host", or the host's own refusal such as "transmitting").
bool host_link_factory_reset(bool logs, char *why, size_t why_len);

// ---- Firmware updates for the main board (docs/architecture/sidekick.md, Firmware updates) ----

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "porta_proto.h"

// The main board's last HELLO: the protocol it speaks, its build version (`version_out` holds
// PORTA_HELLO_VERSION_LEN + 1 bytes) and how long ago it came. False if none has arrived since
// this sidekick started. Any argument may be NULL.
bool host_link_host_hello(uint8_t *proto_out, char *version_out, uint32_t *age_ms_out);

// Puts the main board into update mode, or takes it out. Blocks for the answer, like
// host_link_factory_reset(), and from the httpd task only. false with the reason in `why`.
bool host_link_update_mode(bool on, char *why, size_t why_len);

// The main board's last TX status said it is transmitting.
bool host_link_tx_active(void);

// One firmware message to the main board and its answer. Each blocks up to `timeout_ms`; false
// for no answer. From the update task only, one at a time. host_link_fw_active() brackets a
// transfer so replies are read as they arrive.
void host_link_fw_active(bool active);
bool host_link_fw_begin(const porta_fw_begin_t *begin, int timeout_ms, porta_fw_reply_t *out);
bool host_link_fw_chunk(uint32_t offset, const uint8_t *data, size_t len, int timeout_ms, porta_fw_reply_t *out);
bool host_link_fw_end(bool abort, int timeout_ms, porta_fw_reply_t *out);
