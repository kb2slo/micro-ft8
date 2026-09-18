#include <cstdint>
#include <string>

#include "esp_log.h"

// This board's definitions of the bare-forward-declared seams
// decode_pipeline/ft8_audio_pipeline call but don't own (see the comment
// block at the top of decode_pipeline.cpp for why they're bare declarations
// rather than a shared header). RFC 0005 Phase 2 (this file) is RX decode
// only; TX and the sidekick link are later phases -- see the RFC's Phase 2/3
// split for why arm_from_autoseq_or_beacon and porta_emit_decode are honest
// no-ops here rather than partially-wired.

static const char *TAG = "decode_seams";

// No display on this board -- nothing to push a waterfall row to.
void ui_push_waterfall_row(const uint8_t * /*bins*/, int /*len*/) {}
void ui_mark_rx_list_stale() {}
void ui_set_rx_list_static(const struct RxDecodeEntry * /*entries*/, int /*count*/) {}

// The sidekick link (RFC 0005 Phase 3) hasn't been ported to this board yet;
// safe to no-op unconditionally, same as calling it before porta_start() on
// the ADV.
void porta_emit_decode(uint32_t /*decode_id*/, const char * /*text*/, int /*snr*/, int /*offset_hz*/,
                       float /*dt_s*/, bool /*is_cq*/, bool /*is_to_me*/, bool /*is_recent_qso*/) {}

// No storage on this board (RFC 0005 Phase 2 amendment) -- ESP_LOG is the
// bench-visible substitute for ADV's SD-card RX/TX log during this milestone.
void log_rxtx_line(char dir, int snr, int offset_hz, const std::string &text, int /*repeat_counter*/) {
    ESP_LOGI(TAG, "%c snr=%d offset=%dHz: %s", dir, snr, offset_hz, text.c_str());
}

// TX (RFC 0005 Phase 2's decode/TX core, the next milestone after this one)
// isn't wired on this board yet -- nothing to arm.
void arm_from_autoseq_or_beacon() {}

// No ignorelist storage on this board yet -- honestly "ignore nobody" rather
// than silently always-true/always-false.
bool ignorelist_matches_normalized_dxcall(const std::string & /*dxcall_norm*/) {
    return false;
}
