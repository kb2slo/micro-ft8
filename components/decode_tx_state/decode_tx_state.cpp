#include "decode_tx_state.h"

// Storage previously scattered as bare global definitions across main.cpp.
// volatile qualifiers preserved exactly from the decode_tx_state.h this
// replaces -- see that header's own comments (git history) for why each one
// was volatile: cross-core visibility between the slot loop (core 0) and the
// audio/decode task (core 1), single writer per field, no lock.

static bool    s_decode_enabled = true;
static int     s_time_osr = 2;
static int     s_freq_osr = 2;

static int64_t          s_decode_slot_idx = -1;
static bool             s_capture_clean = false;
static volatile int64_t s_decode_applied_slot_idx = -1;
static volatile bool    s_decode_in_progress = false;

static volatile bool s_tx_active = false;
static volatile bool s_was_txing = false;
// Set on core 0 when TX starts, cleared on core 1 once that slot is skipped.
static volatile int32_t s_tx_slot_idx = -1;

static bool          s_streaming = false;
static volatile bool s_cdc_initial_sync_pending = false;

bool decode_tx_state_get_decode_enabled(void) { return s_decode_enabled; }
void decode_tx_state_set_decode_enabled(bool enabled) { s_decode_enabled = enabled; }
int  decode_tx_state_get_time_osr(void) { return s_time_osr; }
void decode_tx_state_set_time_osr(int osr) { s_time_osr = osr; }
int  decode_tx_state_get_freq_osr(void) { return s_freq_osr; }
void decode_tx_state_set_freq_osr(int osr) { s_freq_osr = osr; }

int64_t decode_tx_state_get_decode_slot_idx(void) { return s_decode_slot_idx; }
void    decode_tx_state_set_decode_slot_idx(int64_t slot_idx) { s_decode_slot_idx = slot_idx; }
bool    decode_tx_state_get_capture_clean(void) { return s_capture_clean; }
void    decode_tx_state_set_capture_clean(bool clean) { s_capture_clean = clean; }
int64_t decode_tx_state_get_decode_applied_slot_idx(void) { return s_decode_applied_slot_idx; }
void    decode_tx_state_set_decode_applied_slot_idx(int64_t slot_idx) { s_decode_applied_slot_idx = slot_idx; }
bool    decode_tx_state_get_decode_in_progress(void) { return s_decode_in_progress; }
void    decode_tx_state_set_decode_in_progress(bool in_progress) { s_decode_in_progress = in_progress; }

bool decode_tx_state_get_tx_active(void) { return s_tx_active; }
void decode_tx_state_set_tx_active(bool active) { s_tx_active = active; }
bool decode_tx_state_get_was_txing(void) { return s_was_txing; }
void decode_tx_state_set_was_txing(bool was_txing) { s_was_txing = was_txing; }
int64_t decode_tx_state_get_tx_slot_idx(void) { return s_tx_slot_idx; }
void    decode_tx_state_set_tx_slot_idx(int64_t slot_idx) { s_tx_slot_idx = (int32_t)slot_idx; }

bool decode_tx_state_get_streaming(void) { return s_streaming; }
void decode_tx_state_set_streaming(bool streaming) { s_streaming = streaming; }
bool decode_tx_state_get_cdc_initial_sync_pending(void) { return s_cdc_initial_sync_pending; }
void decode_tx_state_set_cdc_initial_sync_pending(bool pending) { s_cdc_initial_sync_pending = pending; }
