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

// A request or a Port A wait slower than this is logged (row dlu3, docs/ROADMAP.md): the server runs one
// request at a time, so a slow one holds every other.
#define HOST_LINK_SLOW_MS 500

// Asks the main board to reset its Station settings (and, when `logs`, delete its QSO logs).
// Blocks for the answer. true = the host did it; false = it did not, with the reason in `why`
// ("host link busy", "no reply from host", or the host's own refusal such as "transmitting").
bool host_link_factory_reset(bool logs, char *why, size_t why_len);
