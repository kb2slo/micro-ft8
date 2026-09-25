#pragma once

// ============================================================================
// main_services.h
//
// Small services main.cpp provides to other translation units. Defined in
// main.cpp; declared here once so callers and definition cannot drift.
//
// debug_log_line_public()/log_heap() used to live here too, but moved to the
// debug_log component (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213))) so a headless board's decode
// pipeline can use them without linking main.cpp. ui_mark_config_dirty()
// stays -- it is genuinely ADV's screen redraw nudge, not shared state.
// ============================================================================

// Mark config views dirty so the next UI tick redraws whichever of MENU /
// STATUS is showing. Called from the Station.txt save worker task once a
// write lands.
void ui_mark_config_dirty(void);
