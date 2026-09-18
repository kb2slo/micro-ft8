#include "rtc_core.h"

#include <cstdio>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "external_rtc.h"

static const char* TAG = "RTC_CORE";

bool rtc_valid = false;
time_t rtc_epoch_base = 0;
int64_t rtc_ms_start = 0;
int64_t rtc_last_update = 0;
RtcTimeSource g_rtc_time_source = RtcTimeSource::SAVED;
std::string g_date = "2025-12-11";
std::string g_time = "10:10:00";
bool g_time_synced_from_gps = false;

static void rtc_update_strings_from_epoch(time_t now) {
  struct tm t;
  // UTC on purpose: FT8 slots and ADIF dates are UTC, and the browser sends a
  // UTC epoch. localtime_r would shift the STATUS date/time by the device TZ
  // (often unset, but not something to depend on).
  gmtime_r(&now, &t);
  char buf_date[32];
  snprintf(buf_date, sizeof(buf_date), "%04d-%02d-%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  g_date = buf_date;
  char buf_time[16];
  snprintf(buf_time, sizeof(buf_time), "%02d:%02d:%02d",
           t.tm_hour, t.tm_min, t.tm_sec);
  g_time = buf_time;
}

time_t rtc_current_epoch_seconds() {
  if (!rtc_valid) {
    return (time_t)(esp_timer_get_time() / 1000000);
  }
  return rtc_epoch_base + (esp_timer_get_time() / 1000 - rtc_ms_start) / 1000;
}

void rtc_seed_epoch(time_t epoch, int64_t ms_start, RtcTimeSource source) {
  rtc_epoch_base = epoch;
  rtc_ms_start = ms_start;
  rtc_last_update = ms_start;
  rtc_valid = true;
  g_rtc_time_source = source;
  rtc_update_strings_from_epoch(epoch);
}

bool rtc_set_from_strings_source(RtcTimeSource source) {
  int y, M, d, h, m, s;
  if (sscanf(g_date.c_str(), "%d-%d-%d", &y, &M, &d) != 3) return false;
  if (sscanf(g_time.c_str(), "%d:%d:%d", &h, &m, &s) != 3) return false;
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = M - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_sec = s;
  // Device TZ is unset in normal bring-up, so mktime agrees with the UTC
  // strings STATUS shows (gmtime_r above). Do not invent a TZ here.
  time_t epoch = mktime(&t);
  if (epoch == (time_t)-1) return false;
  rtc_seed_epoch(epoch, esp_timer_get_time() / 1000, source);
  return true;
}

bool rtc_set_from_strings() {
  return rtc_set_from_strings_source(RtcTimeSource::SAVED);
}

void rtc_update_strings() {
  if (!rtc_valid) return;
  rtc_update_strings_from_epoch(rtc_current_epoch_seconds());
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

static external_rtc_datetime_t rtc_external_datetime_from_soft() {
  time_t now = rtc_current_epoch_seconds();
  struct tm t;
  gmtime_r(&now, &t);

  external_rtc_datetime_t datetime = {};
  datetime.year = (uint16_t)(t.tm_year + 1900);
  datetime.month = (uint8_t)(t.tm_mon + 1);
  datetime.day = (uint8_t)t.tm_mday;
  datetime.hour = (uint8_t)t.tm_hour;
  datetime.minute = (uint8_t)t.tm_min;
  datetime.second = (uint8_t)t.tm_sec;
  return datetime;
}

esp_err_t rtc_write_external_from_soft(const char* reason) {
  if (!rtc_valid || !external_rtc_available()) {
    return ESP_ERR_INVALID_STATE;
  }

  external_rtc_datetime_t datetime = rtc_external_datetime_from_soft();
  esp_err_t err = external_rtc_write_datetime(&datetime);
  if (err == ESP_OK) {
    ESP_LOGI(TAG,
             "DS3231 time updated from %s: %04u-%02u-%02u %02u:%02u:%02u",
             reason ? reason : "soft RTC",
             (unsigned)datetime.year,
             (unsigned)datetime.month,
             (unsigned)datetime.day,
             (unsigned)datetime.hour,
             (unsigned)datetime.minute,
             (unsigned)datetime.second);
  } else {
    ESP_LOGW(TAG,
             "DS3231 time update from %s failed: %s",
             reason ? reason : "soft RTC",
             esp_err_to_name(err));
  }
  return err;
}

void rtc_apply_decode_sync(int delta_ms) {
  rtc_ms_start -= delta_ms;
  rtc_last_update -= delta_ms;
  rtc_update_strings();
  rtc_sync_to_esp_rtc();
  (void)rtc_write_external_from_soft("decode auto-sync");
}
