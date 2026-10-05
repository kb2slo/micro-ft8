#pragma once

#include <cstdint>

// ============================================================================
// decode_tx_state.h
//
// The shared contract between the slot loop (main.cpp, core 0) and the
// audio/decode pipeline (main/audio/, core 1), owned here as accessor
// functions instead of shared as bare `extern` globals -- this is the "real
// interface" the original decode_tx_state.h (main/, now retired) named as
// the goal, done as part of docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)) rather than left for later.
//
// Semantics are unchanged from the file this replaces: the same
// single-writer/single-reader access pattern per field, the same `volatile`
// qualifier on the underlying storage wherever the original had one (see
// decode_tx_state.cpp), no new locking introduced. This is a relocation of
// an existing contract, not a redesign of it -- the actual slot-loop/
// decode-pipeline handshake this enables (g_decode_applied_slot_idx gating
// TX for slot N on slot N-1's decode) is unchanged.
// ============================================================================

// --- Decode configuration (read by the pipeline, set from the UI) -----------
bool decode_tx_state_get_decode_enabled(void);
void decode_tx_state_set_decode_enabled(bool enabled);
int  decode_tx_state_get_time_osr(void);
void decode_tx_state_set_time_osr(int osr);
int  decode_tx_state_get_freq_osr(void);
void decode_tx_state_set_freq_osr(int osr);

// --- Decode progress (written by the audio task, read by the slot loop) -----
// decode_slot_idx is tagged at decode trigger so RX lines carry slot parity.
// decode_applied_slot_idx is the monotonic index of the most recent slot
// whose decode has been fully applied to autoseq state (or whose audio was
// never decoded, e.g. paused/skipped) -- written by stream_uac_task on core
// 1, read by check_slot_boundary on core 0, initialized to -1 so the first
// TX after boot isn't blocked. Enforces the sequential invariant "TX in slot
// N is blocked until the decode for slot N-1 has been applied."
int64_t decode_tx_state_get_decode_slot_idx(void);
void    decode_tx_state_set_decode_slot_idx(int64_t slot_idx);
// capture_clean is set with decode_slot_idx, on the same task: the slot began one
// boundary after a whole slot, and the ingest ring dropped nothing from the start of
// that slot to this decode. Only such a slot may step the clock (decode_sync.h).
bool    decode_tx_state_get_capture_clean(void);
void    decode_tx_state_set_capture_clean(bool clean);
int64_t decode_tx_state_get_decode_applied_slot_idx(void);
void    decode_tx_state_set_decode_applied_slot_idx(int64_t slot_idx);
bool    decode_tx_state_get_decode_in_progress(void);
void    decode_tx_state_set_decode_in_progress(bool in_progress);

// --- TX state (written by the slot loop, read by the pipeline) --------------
// was_txing stays set from TX start until the slot loop has ticked autoseq
// after the tones end. That clear is ~10 ms after 12.64 s, the same moment
// the 79th block arrives, so the pipeline does not skip on this flag alone.
bool decode_tx_state_get_tx_active(void);
void decode_tx_state_set_tx_active(bool active);
bool decode_tx_state_get_was_txing(void);
void decode_tx_state_set_was_txing(bool was_txing);
// Slot a transmission started in. The pipeline skips that slot's decode and
// clears it. -1 means none. Stored as 32 bits: a slot index fits, and a
// 32-bit store is one write on the P4.
int64_t decode_tx_state_get_tx_slot_idx(void);
void    decode_tx_state_set_tx_slot_idx(int64_t slot_idx);

// --- Stream state (set by stream_uac.cpp, read by the slot loop) -----------
bool decode_tx_state_get_streaming(void);
void decode_tx_state_set_streaming(bool streaming);
// Set after CDC-ACM opens during an explicit user connection; the slot loop
// consumes it once to synchronize the selected band and mode.
bool decode_tx_state_get_cdc_initial_sync_pending(void);
void decode_tx_state_set_cdc_initial_sync_pending(bool pending);
