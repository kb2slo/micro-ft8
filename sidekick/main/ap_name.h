#pragma once

// ap_name.h -- the sidekick's default AP name, with no ESP-IDF in it
// (docs/architecture/sidekick.md (The sidekick's own network)). Host-tested
// (host_test_ap_name).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "micro-ft8.local" and a space, then the last two bytes of the AP's MAC as
// four uppercase hex digits: "micro-ft8.local 3FA2". The name says the address
// to open, and the suffix is what tells two sidekicks in range apart; it is
// the end of the AP's BSSID, so a WiFi scanner shows the same digits. Writes
// "" when out_len cannot hold the whole name.
#define AP_NAME_DEFAULT_LEN 20  // the name's length, without its NUL
void ap_name_default(const uint8_t ap_mac[6], char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
