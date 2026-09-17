#pragma once

#include <cstdint>

extern "C" {
#include "ft8/message.h"
}

// FT8's compressed-callsign hash table (RFC 0005 Phase 2 -- extracted out of
// main.cpp, which is where kgoba's reference demo also keeps it, but that is
// an artifact of history here, not a real dependency: this table has no UI
// coupling and no board-specific state, and is used by both the decode path
// (resolving a hashed callsign back to text) and the TX path (encoding our
// own messages), via ftx_message_decode()/ftx_message_encode()'s
// ftx_callsign_hash_interface_t argument -- pass &hash_if.

// Reset the table (call once at boot / on RX restart).
void hashtable_init(void);

// Increment age for all existing entries (saturate at 255). Call once per slot.
void hashtable_age_all(void);

// Evict the oldest entries until the table is at or under max_size.
void hashtable_trim_size(int max_size);

// ftx_callsign_hash_interface_t's save_hash callback.
void hashtable_add(const char* callsign, uint32_t hash);

// ftx_callsign_hash_interface_t's lookup_hash callback.
bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char* callsign);

// Pass to ftx_message_decode()/ftx_message_encode() as the hash interface.
extern ftx_callsign_hash_interface_t hash_if;
