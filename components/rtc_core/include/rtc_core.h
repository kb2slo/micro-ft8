#pragma once

#include <cstdint>
#include <ctime>

// ============================================================================
// rtc_core.h
//
// The soft-clock primitives (docs/architecture/time.md): the decode pipeline
// stamps and auto-syncs time through these without linking the main board's
// own file. Arithmetic plus the ESP's built-in RTC peripheral
// (settimeofday/gettimeofday).
// ============================================================================

// Owned state. rtc_valid stays false until the clock is first seeded.
extern bool rtc_valid;
extern time_t rtc_epoch_base;
extern int64_t rtc_ms_start;

int64_t rtc_now_ms();
// For PORTA event stamps: 0 means "clock unset" so the viewer can show
// --:--:-- instead of a 1970-looking fake.
uint32_t rtc_epoch_secs_or_zero();
time_t rtc_current_epoch_seconds();

// Seeds the soft clock from an already-known epoch (a browser sync).
void rtc_seed_epoch(time_t epoch, int64_t ms_start);

// Mirrors the soft clock into the ESP's own built-in RTC peripheral
// (settimeofday) so it survives a warm reset/deep sleep. No-op until valid.
void rtc_sync_to_esp_rtc();

// Nudges the soft clock by delta_ms (positive = clock was behind, negative =
// ahead) and re-syncs the ESP RTC in one step. Used by the decode pipeline's
// auto-sync-from-timing correction.
void rtc_apply_decode_sync(int delta_ms);
