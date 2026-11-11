#pragma once

// fw_update.h -- firmware updates over the air (docs/architecture/sidekick.md
// (Firmware updates)). The sidekick downloads an image itself, over plain
// HTTP, into its spare OTA slot, and refuses it unless its SHA-256 is the one
// the web app read from the release manifest over HTTPS. Its own image it
// installs and restarts into; a main board's it sends down Port A. Every
// decision is in fw_policy.c, which is host-tested; this file is the part
// that needs the network, the flash and a restart, and is verified on
// hardware.

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Reads what the last update left in NVS. Call once, after NVS is ready and
// before any route is registered.
void fw_update_init(void);

// GET /api/fw/status (open); POST /api/fw/start and /api/fw/cancel (pairing
// required).
esp_err_t fw_update_register(httpd_handle_t server);

// Once a second from the main loop. An image installed over the air runs on
// trial until it is marked valid here; one that never earns it restarts, and
// the bootloader goes back to the image before it (fw_confirm_decide()).
void fw_update_tick(uint32_t uptime_s, uint32_t beacons_sent);
