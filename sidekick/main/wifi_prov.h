#pragma once

// ============================================================================
// wifi_prov.h — WiFi bring-up and SoftAP credential provisioning.
//
// RFC 0001 §5.2d: the sidekick pulls its own firmware over HTTPS rather than
// being pushed to over the wire, which means it has to get onto a network
// first. A factory part has no credentials and the ADV cannot supply them
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
// B55 (RFC 0004 §1): the AP raised by steps 1 or 3 is a real operating mode,
// not only a bootstrap state -- it serves the full application (app.html,
// /api/...) alongside the captive-portal credential form at
// http://192.168.4.1/, which now links to both rather than only the form.
// "AP mode always shows the captive portal" is deliberate in every case; what
// changed is that the portal's landing page offers "operate now" as well as
// "join a network instead", and the AP is reachable on purpose, not only by
// accident of no credentials being set.
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

// Deliberately switch into AP mode and restart. Does not return.
//
// Unlike wifi_prov_forget_and_restart(), this does not erase the stored
// credentials -- an operator using this from the station status page still
// wants to be able to rejoin the same network later, which the portal's
// existing WiFi-join form already does with no extra code needed. One-shot
// NVS flag (see wifi_prov.c's NVS_KEY_FORCE_AP), consumed at the top of the
// next wifi_prov_start().
void wifi_prov_enter_ap_and_restart(void);
