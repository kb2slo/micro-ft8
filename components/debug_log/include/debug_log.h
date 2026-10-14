#pragma once

#include <string>

// ============================================================================
// debug_log.h
//
// One funnel for log lines meant for the operator, so the decode pipeline and
// the board's own code reach the sidekick the same way. It keeps no lines of
// its own: the board has no screen to show them, and a buffer here would be
// appended from every logging task at once and need a lock.
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

// Registers a relay called with every logged line. Pass nullptr to clear.
// Last call wins.
void debug_log_set_relay(void (*relay)(const char* text));
