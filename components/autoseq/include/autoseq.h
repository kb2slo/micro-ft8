#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "decode_types.h"

// Queue size: active + inactive entries.
constexpr int AUTOSEQ_MAX_QUEUE = 30;
// Tries are transmissions (docs/architecture/autoseq.md, Tries). The station
// default bounds each QSO message and starts a CQ; an entry's own count can be
// changed in place, up to the per-kind maximum.
constexpr int AUTOSEQ_TRIES = 6;
constexpr int AUTOSEQ_TRIES_MAX_QSO = 20;
constexpr int AUTOSEQ_TRIES_MAX_CQ = 99;
// How long a parked (inactive) entry lives. It is parked to catch DX retrying
// the same QSO; after this, anything from that call is a new QSO, so the old
// state, reports and `logged` must not come back. Equal to the ADIF dedupe
// window (kAdifLoggerDedupeWindowMs; decode_pipeline.cpp asserts it), so a
// call is refused a new QSO for exactly as long as its parked entry lives.
constexpr int64_t AUTOSEQ_INACTIVE_EXPIRY_MS = 10 * 60 * 1000LL;

// High-level auto-sequencer states
// Order matters for priority sorting (higher = more advanced in QSO)
enum class AutoseqState {
    CALLING = 0,   // We sent CQ (TX6)
    REPLYING,      // We sent TX1 (grid)
    REPORT,        // We sent TX2 (SNR report)
    ROGER_REPORT,  // We sent TX3 (R+SNR)
    ROGERS,        // We sent TX4 (RR73)
    SIGNOFF,       // We sent TX5 (73)
    IDLE           // QSO complete (auto-removed)
};

// FT8 message types.
//
// Typing invariant for QsoContext::next_tx:
//
//     next_tx ∈ {TX_NONE, TX1, TX2, TX3, TX4, TX5}
//
// TX6 is the FT8 CQ message but is NEVER assigned to next_tx. CQ ctxs
// (and the Free Text variant that piggy-backs on the CQ infrastructure)
// use next_tx = TX_NONE — text comes from the singleton s_tx_msg_buffer,
// populated at refresh time by either generate_cq_text_into() (for CQ)
// or from the s_pending_ft_text sidecar (for FT).
//
// Semantics of TX_NONE on next_tx:
//   - On a CALLING ctx: "text from s_tx_msg_buffer". The CQ entry stays
//     until its tries are used; a Free Text one-shot is evicted after one TX.
//   - On an IDLE ctx: "no TX, evict immediately by sort_and_clean."
enum class TxMsgType {
    TX_NONE = 0,
    TX1,  // <DXCALL> <MYCALL> <GRID>
    TX2,  // <DXCALL> <MYCALL> ##
    TX3,  // <DXCALL> <MYCALL> R##
    TX4,  // <DXCALL> <MYCALL> RR73
    TX5,  // <DXCALL> <MYCALL> 73
    TX6   // CQ <MYCALL> <GRID> — never used for next_tx (CALLING uses TX_NONE)
};

// QSO context - one per active contact
struct QsoContext {
    AutoseqState state = AutoseqState::IDLE;
    TxMsgType next_tx = TxMsgType::TX_NONE;
    TxMsgType rcvd_msg_type = TxMsgType::TX_NONE;

    // Stable identity for Port A (docs/architecture/webapp.md (Screens) QUEUE_ENTRY/QUEUE_CANCEL):
    // 0 means "not a real entry" (a free slot's default). Assigned once, in
    // append_ctx(), and never reused within a boot -- the queue re-sorts by
    // priority and moves entries around the array via plain struct copies,
    // which already carry this field along for free, so nothing else needs
    // to touch it. A cancel must address this, not a display index: the
    // browser's snapshot and the live queue can disagree on order by the
    // time a cancel arrives.
    uint16_t entry_id = 0;

    std::string dxcall;     // Remote station callsign
    std::string dxgrid;     // Remote grid (preserved from initial exchange!)

    int snr_tx = -99;       // What we report to them (our measurement of their signal)
    int snr_rx = -99;       // What they reported about us

    // A QSO's count is per step: a state change starts it over. A CQ's is the
    // whole CQ: each CQ sent uses one, and an answer gives none back.
    int tries_sent = 0;
    int try_limit = AUTOSEQ_TRIES;
    int tries_custom = 0;   // the entry's own limit; 0 follows the station default
    bool logged = false;    // Prevents duplicate ADIF logging
    bool cabrillo_logged = false; // Prevents duplicate Field Day Cabrillo logging
    bool is_fd = false;
    std::string fd_rx_exchange; // Last received FD exchange (for Cabrillo logging)
    // SIGNOFF handling:
    // true  -> after sending TX5, park context in inactive for possible late RR73
    // false -> after sending TX5, finish immediately (IDLE/pop)
    bool park_after_signoff_tx = false;
    // Timestamp when the context was moved to inactive zone (ms, monotonic).
    // 0 means currently active / not yet parked.
    int64_t inactive_since_ms = 0;

    int offset_hz = 1500;   // TX audio offset
    int slot_id = 0;        // TX slot (0=even, 1=odd)

    // CQ and Free Text entries share the CALLING state but differ in sort
    // priority: FT must TX first (preempts QSOs), CQ stays at bottom (QSOs
    // take priority over a CQ). is_freetext is the priority flag consulted in
    // compare_ctx.
    //
    // Text storage: CQ text is regenerated from template at refresh; FT text
    // is held in the s_pending_ft_text singleton (only one FT pending at a
    // time, so one sidecar suffices). No per-ctx text field is needed.
    bool is_freetext = false;
};

// TX entry for scheduling
struct AutoseqTxEntry {
    std::string text;       // Full FT8 message text
    std::string dxcall;     // Target callsign
    int offset_hz = 1500;
    int slot_id = 0;
    int repeat_counter = AUTOSEQ_TRIES;  // tries left, this one included
    bool is_signoff = false; // True for TX4/TX5 (priority scheduling)
    uint16_t entry_id = 0;   // QsoContext::entry_id of the queue[0] this came from
};

// ADIF logging callback type
using AdifLogCallback = std::function<bool(const std::string& dxcall,
                                            const std::string& dxgrid,
                                            int rst_sent, int rst_rcvd)>;

// ============== Public API ==============

// Initialize/reset the autoseq engine
void autoseq_init();

// Clear all active QSOs
void autoseq_clear();

// Drop a QSO by index (0-based in display order).
// For active QSOs, this moves the context to inactive; CQ entries are removed.
bool autoseq_drop_index(int idx);

// Drop by the stable entry_id above rather than a display index -- what
// Port A's QUEUE_CANCEL addresses. Active zone only; entry_id is not
// exposed for inactive rows. False if no active entry carries that id.
bool autoseq_drop_by_entry_id(uint16_t entry_id);

// Rotate to the next QSO with the same slot parity as the current head.
// Returns true if a rotation occurred.
bool autoseq_rotate_same_parity();

// Call CQ: queue the CQ entry on slot_parity (0 even, 1 odd) with the station
// default's tries. It stays queued, below every QSO, until its tries are used
// or it is dropped. False if a CQ is already queued (it is left as it is) or
// there is no room.
bool autoseq_call_cq(int slot_parity);

// The parity Call CQ picks: an active QSO's if there is one, so the CQ keeps
// the period the station is already using; otherwise the slot that is under
// way when it can still be keyed, or the next one.
int autoseq_cq_parity(int current_slot_parity, bool current_slot_keyable);

// The queued CQ's entry_id, 0 if none.
uint16_t autoseq_cq_entry_id();

// Schedule a Free Text one-shot transmission.
// - Inherits slot parity from queue[0] if queue is non-empty, so FT joins the
//   current activation period instead of colliding with other QSOs' slots.
// - If queue is empty, uses fallback_slot_parity (caller should pass the next
//   TX slot's parity based on wall-clock).
// Returns false if no room or text is empty.
bool autoseq_schedule_freetext(const std::string& text, int fallback_slot_parity);

// Manual response: user taps on a decoded message.
// Prevents duplicate active QSOs for the same DX callsign: promotes a fresh
// entry to the front, or ignores the tap when that call is already the live
// queue head (REPLYING..SIGNOFF). hold_queue_head (TX in flight) promotes
// behind queue[0] so the in-flight QSO is not displaced.
enum class AutoseqTouchResult : uint8_t {
    Queued = 0,
    IgnoredInProgress,
    NoRoom,
};
AutoseqTouchResult autoseq_on_touch(const UiRxLine& msg);
AutoseqTouchResult autoseq_on_touch(const UiRxLine& msg, bool hold_queue_head);

// Automatic response: process all decoded messages addressed to us
void autoseq_on_decodes(const std::vector<UiRxLine>& to_me_messages);

// TX retry tick - call AFTER TX completes to set up retry
// This advances retry counter and sets next_tx for the next attempt
void autoseq_tick(int64_t slot_idx, int slot_parity, int ms_to_boundary);

// Get next TX text based on current state (does NOT modify state)
bool autoseq_get_next_tx(std::string& out_text);

// Fetch pending TX entry based on current state (does NOT modify state)
bool autoseq_fetch_pending_tx(AutoseqTxEntry& out);

// entry_id of the entry autoseq would transmit next, 0 if none. A board
// compares it with its armed TX's entry_id before keying: a queue change that
// did not re-arm (a cancel, a drop) would otherwise send the old message.
uint16_t autoseq_head_entry_id();

// Change one queued entry's tries in place (docs/architecture/autoseq.md,
// Tries). 0 returns it to the station default.
enum class AutoseqTriesResult : uint8_t {
    Ok = 0,
    UnknownEntry,
    OutOfRange,
};
AutoseqTriesResult autoseq_set_entry_tries(uint16_t entry_id, int tries);

// Move the queued CQ to the other period. Only a CQ: a QSO's parity is set by
// the station it works.
enum class AutoseqParityResult : uint8_t {
    Ok = 0,
    UnknownEntry,
    NotCq,
};
AutoseqParityResult autoseq_set_cq_parity(uint16_t entry_id, int slot_parity);

// Combines the entry_id check above with the text refresh a board's armed
// snapshot also needs: the *same* queue[0] entry's rendered text can change
// without its entry_id changing (a live cq_type switch re-derives the CQ
// template for the entry already at CALLING, RFC 0009 §4.4) — a bare
// entry_id comparison alone misses that, so a switch made after a board
// armed a slot but before it fired would silently transmit the old text.
// Returns true if the head entry's identity changed (the caller must drop
// this arm and re-arm from scratch, as it already does on the old check);
// false if it's still the same entry, with `pending->text` refreshed in
// place from the live buffer (everything else about `*pending`, including
// the offset resolved at arm time, is left untouched).
bool autoseq_head_changed_or_refresh(AutoseqTxEntry* pending);

// entry_id of the entry whose TX started last (autoseq_on_tx_starting), until
// the tick after that TX; 0 if none. Only meaningful while a TX is on the air:
// a cancelled TX gets no tick, so the board gates this on its own tx_active.
// Cancelling that entry from the queue also stops the transmission.
uint16_t autoseq_transmitting_entry_id();

// A TX that was on the air was cancelled: it is not counted and gets no tick,
// so nothing is transmitting until the next autoseq_on_tx_starting().
void autoseq_on_tx_cancelled();

// Mark TX as sent (called after transmission completes)
void autoseq_mark_sent(int64_t slot_idx);

// Called from tx_start() immediately before TX emission begins.
// Logs the QSO if we're about to emit TX4 (RR73) or TX5 (73) — this is the
// single logging trigger. ctx->logged flag prevents duplicate logs across
// retries or re-emissions.
void autoseq_on_tx_starting();

// Get display strings for active QSOs
void autoseq_get_qso_states(std::vector<std::string>& out);

// Copy the full active-zone QsoContext entries for structured consumers
// (e.g. the core_api). Returns active contexts only; inactive
// zone is not included. Thread-safe snapshot.
void autoseq_get_active_contexts(std::vector<QsoContext>& out);

// Non-allocating accessors for hot paths that can't tolerate
// std::vector::reserve under heap fragmentation.
int  autoseq_active_count();
bool autoseq_get_active_context(int idx, QsoContext* out);

// Check if there's any active QSO (not IDLE)
bool autoseq_has_active_qso();

// Get current queue size
int autoseq_queue_size();

// Set the ADIF logging callback
void autoseq_set_adif_callback(AdifLogCallback cb);

// Cabrillo Field Day callback type (for ARRL-FD logging)
using CabrilloFdLogCallback = bool (*)(const std::string& dxcall, const std::string& their_fd_exchange);
void autoseq_set_cabrillo_fd_callback(CabrilloFdLogCallback cb);

// "Worked this call recently?" -- true stops a decode from a caller with no
// queue entry (active or inactive) starting a new QSO, so a station just
// worked is not worked twice. Messages for an entry already in the queue are
// never filtered: a QSO is logged when we send RR73, so the station's 73 or a
// resent R+report arrives after it is "recent" and must still reach its
// entry. A touch is not filtered either. dxcall_norm is upper case with <>
// stripped. Null (the default) filters nothing.
using AutoseqRecentQsoCallback = bool (*)(const std::string& dxcall_norm);
void autoseq_set_recent_qso_callback(AutoseqRecentQsoCallback cb);

// "What grid did this call last send?" -- consulted only when a QSO is logged
// with no grid of its own (none sent in the QSO or in the CQ we tapped).
// True fills *grid_out. dxcall_norm is upper case with <> stripped. Null (the
// default) logs such a QSO with no grid.
using AutoseqHeardGridCallback = bool (*)(const std::string& dxcall_norm, std::string* grid_out);
void autoseq_set_heard_grid_callback(AutoseqHeardGridCallback cb);

// Configuration setters (called when station data changes)
void autoseq_set_station(const std::string& call, const std::string& grid);
void autoseq_set_skip_tx1(bool skip);  // Skip TX1 and start with TX2
// Station default tries, 1..AUTOSEQ_TRIES_MAX_QSO. Applies at once to every
// entry that has no count of its own, the queued CQ included.
void autoseq_set_tries(int tries);

// CQ type configuration
enum class AutoseqCqType { CQ = 0, SOTA, POTA, QRP, FD, FREETEXT };
void autoseq_set_cq_type(AutoseqCqType type, const std::string& freetext = "");
