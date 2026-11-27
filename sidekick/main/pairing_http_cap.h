#pragma once

// Compile-time capacity for pairing_http's route-slot table.
//
// Policy still sits beside each handler at the pairing_http_register() call
// site (pairing_http.h). This header only answers "how many slots": each
// module owns a named count and _Static_assert's its local routes[] length
// against it. Add a route → bump that module's count (or the assert fails).
// Bump the count without adding → assert fails the other way when the array
// is shorter. The table size is the larger of the two httpd modes.

enum {
    PAIRING_ROUTES_DISCLOSURE = 1,  // /api/pairing-token

    PAIRING_ROUTES_WIFI_PROV_AP_CORE = 6,       // /  /rescan  /update  /provision  /factory-reset  /ap-config
    PAIRING_ROUTES_WIFI_PROV_STATION_CORE = 6,  // /  /update  /forget  /ap-mode  /factory-reset  /ap-config

    PAIRING_ROUTES_HOST_LINK = 23,  // 17th/18th: POST /api/queue/tries and /api/queue/parity; 19th: POST /api/radio/read; 20th: GET /api/state; 21st: POST /api/radio/ptt; 22nd: POST /api/radio/setting; 23rd: GET /api/qrz_key; PATCH /api/log/entries took GET /api/log/files's place
    PAIRING_ROUTES_FW_UPDATE = 3,  // GET /api/fw/status, POST /api/fw/start and /api/fw/cancel
    // AP mode serves the full application, the same as station mode
    // (docs/architecture/system.md (Constraints)), so both register the same routes below.
    PAIRING_ROUTES_APP_SERVE = 2,  // POST /api/app/push, GET /*

    PAIRING_HTTP_MAX_ROUTES_AP = PAIRING_ROUTES_WIFI_PROV_AP_CORE +
        PAIRING_ROUTES_DISCLOSURE + PAIRING_ROUTES_HOST_LINK +
        PAIRING_ROUTES_FW_UPDATE + PAIRING_ROUTES_APP_SERVE,

    PAIRING_HTTP_MAX_ROUTES_STATION =
        PAIRING_ROUTES_WIFI_PROV_STATION_CORE + PAIRING_ROUTES_DISCLOSURE +
        PAIRING_ROUTES_HOST_LINK + PAIRING_ROUTES_FW_UPDATE +
        PAIRING_ROUTES_APP_SERVE,

    PAIRING_HTTP_MAX_ROUTES =
        (PAIRING_HTTP_MAX_ROUTES_STATION > PAIRING_HTTP_MAX_ROUTES_AP)
            ? PAIRING_HTTP_MAX_ROUTES_STATION
            : PAIRING_HTTP_MAX_ROUTES_AP,
};
