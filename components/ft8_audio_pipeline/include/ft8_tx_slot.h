#pragma once

#include <stdint.h>

// Which of a channel's slots our transmission makes it deaf in, and which applied mark a skipped or decoded
// slot sets for the TX gate (decode_tx_state.h; docs/architecture/decode.md, Deaf by overlap). Every FT4
// channel counts in FT4 periods, the station's alone or the one beside FT8, and FT8's station channel in
// FT8 slots; a channel's slot index is in its own mode's units, so it is compared with the index in those.

// Whether slot_idx is one our transmission covered, so its decode is skipped.
bool ft8_tx_slot_is_tx(bool ft4, bool station, int64_t slot_idx);
// Clears the transmission's index for this channel once its slot is skipped.
void ft8_tx_slot_clear(bool ft4, bool station, int64_t slot_idx);
// Marks slot_idx applied for the TX gate: FT4's period mark for any FT4 channel, the station's slot mark for
// the station's channel. Another channel touches neither.
void ft8_tx_slot_mark_applied(bool ft4, bool station, int64_t slot_idx);
