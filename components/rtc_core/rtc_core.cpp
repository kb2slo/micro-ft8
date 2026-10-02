#include "rtc_core.h"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"


static const char* TAG = "RTC_CORE";

bool rtc_valid = false;
time_t rtc_epoch_base = 0;
int64_t rtc_ms_start = 0;

time_t rtc_current_epoch_seconds() {
  if (!rtc_valid) {
    return (time_t)(esp_timer_get_time() / 1000000);
  }
  return rtc_epoch_base + (esp_timer_get_time() / 1000 - rtc_ms_start) / 1000;
}

void rtc_seed_epoch(time_t epoch, int64_t ms_start) {
  rtc_epoch_base = epoch;
  rtc_ms_start = ms_start;
  rtc_valid = true;
}

int64_t rtc_now_ms() {
  if (!rtc_valid) {
    return esp_timer_get_time() / 1000;
  }
  return (int64_t)rtc_epoch_base * 1000 + (esp_timer_get_time() / 1000 - rtc_ms_start);
}

uint32_t rtc_epoch_secs_or_zero() {
  if (!rtc_valid) {
    return 0;
  }
  return (uint32_t)(rtc_now_ms() / 1000);
}

void rtc_sync_to_esp_rtc() {
  if (!rtc_valid) return;

  time_t now = rtc_current_epoch_seconds();
  struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  ESP_LOGI(TAG, "ESP RTC synced from soft RTC");
}

void rtc_apply_decode_sync(int delta_ms) {
  rtc_ms_start -= delta_ms;
  rtc_sync_to_esp_rtc();
}
