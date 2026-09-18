#include <cstdint>
#include <string>

#include "esp_log.h"
#include "tx_scheduler.h"

// This board's definitions of the bare-forward-declared seams
// decode_pipeline/ft8_audio_pipeline call but don't own (see the comment
// block at the top of decode_pipeline.cpp for why they're bare declarations
// rather than a shared header).

static const char *TAG = "decode_seams";

void ui_push_waterfall_row(const uint8_t * /*bins*/, int /*len*/) {}
void ui_mark_rx_list_stale() {}
void ui_set_rx_list_static(const struct RxDecodeEntry * /*entries*/, int /*count*/) {}

void log_rxtx_line(char dir, int snr, int offset_hz, const std::string &text, int /*repeat_counter*/) {
    ESP_LOGI(TAG, "%c snr=%d offset=%dHz: %s", dir, snr, offset_hz, text.c_str());
}

void arm_from_autoseq_or_beacon() {
    tx_scheduler_arm_from_autoseq_or_beacon();
}

bool ignorelist_matches_normalized_dxcall(const std::string & /*dxcall_norm*/) {
    return false;
}
