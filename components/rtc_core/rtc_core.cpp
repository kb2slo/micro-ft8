#include "rtc_core.h"

#include <atomic>
#include <climits>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"


static const char* TAG = "RTC_CORE";

// UTC ms minus esp_timer ms, or kUnset until the first set. One value, so one
// load reads a whole set: every task reads the clock, and two tasks change it
// (a set from the phone on the main loop, auto-sync on the decode task). On
// the P4 a 64-bit atomic is ESP-IDF's spinlock emulation (newlib stdatomic.c).
static constexpr int64_t kUnset = INT64_MIN;
static std::atomic<int64_t> s_offset_ms{kUnset};

static int64_t uptime_ms() {
  return esp_timer_get_time() / 1000;
}

bool rtc_is_set() {
  return s_offset_ms.load() != kUnset;
}

time_t rtc_current_epoch_seconds() {
  const int64_t offset = s_offset_ms.load();
  if (offset == kUnset) {
    return (time_t)(esp_timer_get_time() / 1000000);
  }
  return (time_t)((uptime_ms() + offset) / 1000);
}

void rtc_seed_epoch(time_t epoch, int64_t ms_start) {
  s_offset_ms.store((int64_t)epoch * 1000 - ms_start);
}

int64_t rtc_now_ms() {
  const int64_t offset = s_offset_ms.load();
  if (offset == kUnset) {
    return uptime_ms();
  }
  return uptime_ms() + offset;
}

uint32_t rtc_epoch_secs_or_zero() {
  const int64_t offset = s_offset_ms.load();
  if (offset == kUnset) {
    return 0;
  }
  return (uint32_t)((uptime_ms() + offset) / 1000);
}

void rtc_sync_to_esp_rtc() {
  if (!rtc_is_set()) return;

  time_t now = rtc_current_epoch_seconds();
  struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  ESP_LOGI(TAG, "ESP RTC synced from soft RTC");
}

void rtc_apply_decode_sync(int delta_ms) {
  // A compare-and-swap: a set landing between this load and the swap makes
  // the swap fail, so the step goes on top of that set instead of writing
  // over it. A step that finishes first is replaced by the set, as any set
  // replaces the clock whole. An unset clock reads uptime and has nothing to
  // step; the first set replaces it whole.
  int64_t offset = s_offset_ms.load();
  while (offset != kUnset && !s_offset_ms.compare_exchange_weak(offset, offset + delta_ms)) {
  }
  rtc_sync_to_esp_rtc();
}
