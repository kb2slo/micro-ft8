#pragma once

// ============================================================================
// wifi_prov.h — WiFi bring-up and SoftAP credential provisioning.
//
// The sidekick joins a network so the phone can reach it, and so it can
// download its own firmware (docs/architecture/sidekick.md, Firmware updates:
// plain HTTP, hash-checked, not yet built). A factory part has no credentials and the ADV cannot supply them
// (that would need Station.txt fields and menu space that does not exist), so
// the sidekick provisions itself through the browser UI it has to serve
// anyway.
//
// Boot sequence:
//   1. an operator-forced AP-mode flag is set (see wifi_prov_enter_ap_and_
//      restart(), below) -> clear the flag and raise the AP directly,
//      skipping station join entirely
//   2. credentials in NVS -> try to join, retry a few times
//   3. no credentials, or the join fails -> raise an open SoftAP
//   4. form submitted -> store to NVS, try to join, report the result
//
// B55 (docs/architecture/system.md (Constraints)): the AP raised by steps 1 or 3 is a real operating mode,
// not only a bootstrap state -- it serves the full application (app.html,
// /api/...) alongside the credential form at http://192.168.4.1/
// (provision.html), which now links to both rather than only the form. The
// AP is reachable on purpose, not only by accident of no credentials being
// set. B74 follow-up (2026-09-21): there is no auto-popping OS captive-
// portal sign-in sheet any more -- that mechanism (DHCP option 114, a DNS-
// hijack server, and a "resolve to Done" probe-response dance in web_fs.c)
// never reliably resolved to "Done" on iOS regardless of tuning, and was the
// actual source of the cellular-retention trade-off this file used to fight
// over DHCP options. An operator navigates to http://micro-ft8.local/
// manually -- the AP's own SSID already says exactly that.
//
// The PORTA beacon is independent of all of this and keeps running throughout
// (different peripheral, different task) — a sidekick with no WiFi is still a
// working companion, just not a self-updating one.
// ============================================================================

#include <stdbool.h>

// Brings up NVS, netif, the event loop and WiFi, then runs the sequence above.
// Returns once the outcome is settled: either joined, or the AP is up and
// serving. Never blocks forever — a failed join falls back to the AP rather
// than retrying indefinitely.
void wifi_prov_start(void);

// True once an IP has been obtained on the station interface.
bool wifi_prov_is_online(void);

// SSID currently joined, or "" when offline. Points at static storage.
const char* wifi_prov_ssid(void);

// True while the provisioning AP is up.
bool wifi_prov_ap_active(void);

// Whichever-mode-is-actually-active network facts, surfaced through
// /api/config (host_link.c) for app.html's Settings > Network section --
// retired from the old status.html, which read this same state directly.
// Each points at static storage, recomputed on every call.
const char *wifi_prov_network_name(void);  // joined SSID, or the AP's own SSID
const char *wifi_prov_ip_str(void);        // station IP, or the AP gateway IP
const char *wifi_prov_uptime_str(void);    // "3h 12m", same shape as app.html's fmtAgo
const char *wifi_prov_fw_version(void);    // exact git SHA + dirty flag (PROJECT_VER)

// Hostname the sidekick answers to over mDNS, in both AP and station mode:
// http://micro-ft8.local/ . Without it the operator has to read an IP off a
// serial console -- which is precisely the console the finished product will
// not have attached.
#define WIFI_PROV_MDNS_HOST "micro-ft8"

// Forget the stored network and restart into provisioning. Does not return.
//
// Credentials live in exactly one place -- this module's NVS keys -- because
// wifi_prov_start() puts the driver in WIFI_STORAGE_RAM. Without that the
// driver keeps its own copy in nvs.net80211, and a "forget" clearing only our
// keys would leave the device reconnecting to the network it was told to
// forget.
void wifi_prov_forget_and_restart(void);

// Returns the sidekick to a fresh-from-flash state and restarts. Does not
// return. Erases everything stored in the "sidekick" NVS namespace (WiFi
// credentials, the AP-mode flag, the pairing token, the bundle host) and
// formats the web filesystem, so the installed app is gone and only the
// firmware's seed pages come back. Every browser that had paired must pair
// again. The main board's own settings are not touched.
void wifi_prov_factory_reset_and_restart(void);

// Deliberately switch into AP mode and restart. Does not return.
//
// Unlike wifi_prov_forget_and_restart(), this does not erase the stored
// credentials -- an operator using this from the station status page still
// wants to be able to rejoin the same network later, which the portal's
// existing WiFi-join form already does with no extra code needed. One-shot
// NVS flag (see wifi_prov.c's NVS_KEY_FORCE_AP), consumed at the top of the
// next wifi_prov_start().
void wifi_prov_enter_ap_and_restart(void);
