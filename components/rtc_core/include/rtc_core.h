#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "esp_err.h"

// ============================================================================
// rtc_core.h
//
// The soft-clock primitives (RFC 0005 Phase 2 -- extracted out of main.cpp so
// the decode pipeline can stamp/auto-sync time without linking main.cpp).
// This is arithmetic plus the ESP's own built-in RTC peripheral
// (settimeofday/gettimeofday, present on every ESP32) -- no board coupling of
// its own. The DS3231 external-chip write goes through external_rtc.h, whose
// P4 implementation honestly reports "not present"; the primitives here don't
// care which board they're on.
//
// What stayed in main.cpp: multi-source arbitration triggers (GPS fix
// handling, DS3231/ESP-RTC boot-time init, manual keyboard entry, browser/
// phone sync via porta) -- those are ADV-UI/GPS/porta-host-command callers of
// these primitives, not primitives themselves. A board with no GPS/DS3231/
// keyboard (P4, today) simply never calls those triggers; the soft clock
// still works, degrading to monotonic-since-boot exactly like ADV does with
// no fix yet -- not a new state, the one this file already had.
// ============================================================================

enum class RtcTimeSource : uint8_t {
  SAVED = 0,
  ESP_RTC,
  DS3231,
  GPS,
  MANUAL,
  PHONE,
  BROWSER,   // set over the companion link from a phone or laptop browser
};

// Owned state. g_date/g_time are UTC "YYYY-MM-DD"/"HH:MM:SS", kept in sync
// with the epoch on every seed/update -- ADV's STATUS screen reads them for
// display and manual-entry editing; a board with no screen can ignore them.
extern bool rtc_valid;
extern time_t rtc_epoch_base;
extern int64_t rtc_ms_start;
extern int64_t rtc_last_update;
extern RtcTimeSource g_rtc_time_source;
extern std::string g_date;
extern std::string g_time;
extern bool g_time_synced_from_gps;

int64_t rtc_now_ms();
// For PORTA event stamps: 0 means "clock unset" so the viewer can show
// --:--:-- instead of a 1970-looking fake.
uint32_t rtc_epoch_secs_or_zero();
time_t rtc_current_epoch_seconds();

// Seeds the soft clock from an already-known epoch (GPS fix, DS3231 read,
// manual entry, browser sync, ...). Updates g_date/g_time to match.
void rtc_seed_epoch(time_t epoch, int64_t ms_start, RtcTimeSource source);

// Parses g_date/g_time (as UTC) and seeds the soft clock from them, tagging
// the source. False if the strings don't parse.
bool rtc_set_from_strings_source(RtcTimeSource source);
bool rtc_set_from_strings();

// Re-derives g_date/g_time from the current epoch. No-op until rtc_valid.
void rtc_update_strings();

// Mirrors the soft clock into the ESP's own built-in RTC peripheral
// (settimeofday) so it survives a warm reset/deep sleep. No-op until valid.
void rtc_sync_to_esp_rtc();

// Writes the soft clock to the DS3231 (external_rtc.h) if this board has one
// and it's actually present -- a no-op (ESP_ERR_INVALID_STATE) otherwise,
// same as a board that has the chip but hasn't found it yet on the bus.
esp_err_t rtc_write_external_from_soft(const char* reason);

// Nudges the soft clock by delta_ms (positive = clock was ahead, negative =
// behind) and re-syncs everything downstream (strings, ESP RTC, DS3231 if
// present) in one step. Used by the decode pipeline's auto-sync-from-timing
// correction -- callers used to poke rtc_ms_start/rtc_last_update directly
// and call the three re-sync steps themselves; this is that, named.
void rtc_apply_decode_sync(int delta_ms);
