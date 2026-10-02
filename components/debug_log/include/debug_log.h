#pragma once

#include <string>
#include <vector>

// ============================================================================
// debug_log.h
//
// The on-screen "D" debug log ring buffer (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)) -- extracted out
// of main.cpp so a headless board's decode pipeline can log through the same
// funnel without linking main.cpp itself). No board/UI coupling of its own:
// g_debug_lines/debug_page are plain data a screen *may* choose to render
// (the board has no screen, so nothing reads them today).
//
// Relaying lines to the sidekick (porta_emit_log) used to be hardwired here,
// but that made this component depend on porta and porta depend on this one
// (porta.cpp already calls debug_log_line_public for pairing status lines).
// debug_log_set_relay() breaks the cycle: whoever owns the relay registers
// once, and this file has no compile-time knowledge of porta at all. Nothing
// registers on a board that never calls porta_start() -- same effective
// no-relay behavior porta's own s_running guard already gives you.
// ============================================================================

void debug_log_line(const std::string& msg);
// Same, callable from other translation units (this file is the only .cpp
// allowed to define debug_log_line itself as file-local; everything else
// goes through this).
void debug_log_line_public(const std::string& msg);

// Log free / minimum-free / largest-block heap figures under `tag`.
void log_heap(const char* tag);

// Registers a relay called with every logged line, after it's appended to
// g_debug_lines. Pass nullptr to clear. Last call wins.
void debug_log_set_relay(void (*relay)(const char* text));

extern std::vector<std::string> g_debug_lines;
extern int debug_page;
extern const size_t DEBUG_MAX_LINES;  // 3 pages
extern const size_t DEBUG_HUD_LINES;  // slots 0-1 reserved for live HUD

// Ensures the HUD-reserved slots (0..DEBUG_HUD_LINES-1) exist. Called by
// debug_log_line() itself and by whoever fills the HUD slots (main.cpp's
// anything that samples the stack); a no-op if nothing does.
void debug_ensure_hud_lines();
