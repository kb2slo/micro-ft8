#pragma once

#include <cstdint>
#include <string>

#include "decode_types.h"

#include "ft8_audio_pipeline.h"

// The decode/autoseq-arm/RTC-auto-sync pipeline (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)) -- extracted
// out of main.cpp; this is what P4's "RX decode" milestone build-forces into
// components/, per STYLE's module-placement rule). Called once per slot of each channel by
// ft8_audio_pipeline_run()'s decode task (ft8_decode, core 1), with the channel's finished slot. Decodes it
// with the channel's own decode state (decode_core.h) and steps the clock on the decode task,
// then hands the slot's decodes to decode_pipeline_apply().
void decode_slot(const ft8_slot_t* slot);

// The main loop's half of each decode: applies each channel's slot decode_slot()
// last handed over -- the heard-grid and recently-worked tables, the sort,
// autoseq_on_decodes(), arming, the decode ids and porta_emit_decode() -- so the
// decode task touches none of them. Call it on every pass of the main loop,
// before the slot cycle.
void decode_pipeline_apply(void);

// True while the channel that gates this transmission has a decoded slot waiting for decode_pipeline_apply():
// the station's for FT8, the FT4 channel beside it for FT4. The TX trigger holds on it as on
// decode_in_progress: a slot's decode is not applied until the main loop has applied it.
bool decode_pipeline_result_pending(bool ft4);

// The station call the decode task tests decodes against (addressed to us,
// the DXpedition rewrite). The main loop sets it whenever the call changes;
// the decode task reads its own copy, never the board's g_call. Empty until
// set, which matches nothing.
void decode_pipeline_set_my_call(const std::string& call);

// Resolves a decode_id handed out by an earlier decode_pipeline_apply() call
// (see porta_emit_decode) back to the UiRxLine a QUEUE_REPLY names. Main loop.
bool find_decode_for_reply(uint32_t decode_id, UiRxLine* out);

// True if dxcall_norm was logged within the ADIF dedupe window (10 min) on the radio's band in this mode. Each
// board registers it with autoseq_set_recent_qso_callback(), which consults it
// only before starting a new QSO.
bool decode_pipeline_recently_logged(const std::string& dxcall_norm, RxMode mode);

// The grid dxcall_norm last sent in any decode (heard_grid.h). Each board
// registers it with autoseq_set_heard_grid_callback(), which consults it only
// when a QSO logs with no grid of its own.
bool decode_pipeline_heard_grid(const std::string& dxcall_norm, std::string* grid_out);
