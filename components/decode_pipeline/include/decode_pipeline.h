#pragma once

#include <cstdint>
#include <string>

#include "decode_types.h"

extern "C" {
#include "common/monitor.h"
}

// The decode/autoseq-arm/RTC-auto-sync pipeline (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)) -- extracted
// out of main.cpp; this is what P4's "RX decode" milestone build-forces into
// components/, per STYLE's module-placement rule). Called once per slot by
// ft8_audio_pipeline_run() with a filled monitor_t, and the AGC gain it applied to
// each waterfall block (decode_snr.h).
void decode_monitor_results(monitor_t* mon, const monitor_config_t* cfg, const float* block_gain);

// Resolves a decode_id handed out by an earlier decode_monitor_results() call
// (see porta_emit_decode) back to the UiRxLine a QUEUE_REPLY names.
bool find_decode_for_reply(uint32_t decode_id, UiRxLine* out);

// True if dxcall_norm was logged within the ADIF dedupe window (10 min). Each
// board registers it with autoseq_set_recent_qso_callback(), which consults it
// only before starting a new QSO.
bool decode_pipeline_recently_logged(const std::string& dxcall_norm);
