#include <cstdint>
#include <string>

#include "esp_log.h"

// This board's definitions of the bare-forward-declared seams
// decode_pipeline/ft8_audio_pipeline call but don't own (see the comment
// block at the top of decode_pipeline.cpp for why they're bare declarations
// rather than a shared header). porta_emit_decode comes from components/porta
// once porta_start() has run (B67); TX arming remains a no-op until B68.

static const char *TAG = "decode_seams";

// No display on this board -- nothing to push a waterfall row to.
void ui_push_waterfall_row(const uint8_t * /*bins*/, int /*len*/) {}
void ui_mark_rx_list_stale() {}
void ui_set_rx_list_static(const struct RxDecodeEntry * /*entries*/, int /*count*/) {}

// No storage on this board (RFC 0005 Phase 2 amendment) -- ESP_LOG is the
// bench-visible substitute for ADV's SD-card RX/TX log during this milestone.
void log_rxtx_line(char dir, int snr, int offset_hz, const std::string &text, int /*repeat_counter*/) {
    ESP_LOGI(TAG, "%c snr=%d offset=%dHz: %s", dir, snr, offset_hz, text.c_str());
}

// TX (B68) isn't wired on this board yet -- nothing to arm.
void arm_from_autoseq_or_beacon() {}

// No ignorelist storage on this board yet -- honestly "ignore nobody" rather
// than silently always-true/always-false.
bool ignorelist_matches_normalized_dxcall(const std::string & /*dxcall_norm*/) {
    return false;
}
