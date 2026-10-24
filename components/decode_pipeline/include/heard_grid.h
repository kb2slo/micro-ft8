#pragma once

#include <cstdint>
#include <string>

// The grid each station last sent in its own message, so a QSO that never
// carried one (tail-ending an RR73 or a report, or a tapped CQ whose entry
// ran out of tries before DX answered) still logs a grid
// (docs/architecture/autoseq.md, Classifying what DX sent). Fed from every
// decode; read by autoseq when it logs, through
// autoseq_set_heard_grid_callback(). Times are uptime: a reboot empties the
// table, so it never depends on the clock having been set. Not locked: every
// caller is on the main loop (decode_pipeline_apply() notes each decode, and
// autoseq reads it when a QSO logs).

constexpr int HEARD_GRID_CAPACITY = 128;
constexpr int64_t HEARD_GRID_EXPIRY_MS = 4LL * 60 * 60 * 1000;

void heard_grid_clear();

// A decode's sender (field2, decode_fields.h) and the field after it.
// standard_message is ft8_lib's FTX_MESSAGE_TYPE_STANDARD (i3 1 or 2), the only
// messages whose grid field is the sender's own; free text can spell anything.
// Records nothing unless the message is standard, the field after the sender
// is a grid (not RR73), and the sender looks like a callsign. A call already
// in the table takes the new grid and time; a full table drops the call heard
// longest ago.
void heard_grid_note(const char* sender, const char* field3, bool standard_message, int64_t now_ms);

// The grid `call` last sent, if heard within HEARD_GRID_EXPIRY_MS of now_ms.
bool heard_grid_lookup(const char* call, int64_t now_ms, std::string* grid_out);
