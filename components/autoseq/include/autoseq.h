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
// An opening call (REPLYING: our grid to a station that has not answered us)
// stops here even when the default is higher; an entry's own count overrides it.
constexpr int AUTOSEQ_OPENING_TRIES = 4;
constexpr int AUTOSEQ_TRIES_MAX_QSO = 20;
constexpr int AUTOSEQ_TRIES_MAX_CQ = 99;
// How long a parked (inactive) entry lives. It is parked to catch DX retrying
// the same QSO; after this, anything from that call is a new QSO, so the old
// state, reports and `logged` must not come back. Equal to the ADIF dedupe
// window (kAdifLoggerDedupeWindowMs; decode_pipeline.cpp asserts it), so a
// call is refused a new QSO for exactly as long as its parked entry lives.
constexpr int64_t AUTOSEQ_INACTIVE_EXPIRY_MS = 10 * 60 * 1000LL;
// The busy hold (docs/architecture/autoseq.md, The busy hold): a station heard continuing a QSO with
// another call within this many of its channel's slots is busy, and an opening call to it waits.
constexpr int AUTOSEQ_BUSY_SLOTS = 4;
// A hold this long ends the entry, as if its last try were used.
constexpr int64_t AUTOSEQ_HOLD_MAX_MS = 5 * 60 * 1000LL;
// Stations whose newest move is kept for it; a full table drops the one heard longest ago.
constexpr int AUTOSEQ_HEARD_CAPACITY = 64;

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
// TX6 is the FT8 CQ message but is NEVER assigned to next_tx. The CQ ctx
// uses next_tx = TX_NONE — text comes from the singleton s_tx_msg_buffer,
// populated at refresh time by generate_cq_text_into().
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
    // Every transmission of a QSO over its life, each step and each reactivation counted, never started over: the
    // TX watchdog's bound on a QSO that keeps changing step (tx_watchdog.h, autoseq_tick()). A CQ's tries are its own.
    int tx_total = 0;
    bool logged = false;    // Prevents duplicate ADIF logging
    // SIGNOFF handling:
    // true  -> after sending TX5, park context in inactive for possible late RR73
    // false -> after sending TX5, finish immediately (IDLE/pop)
    bool park_after_signoff_tx = false;
    // Timestamp when the context was moved to inactive zone (ms, monotonic).
    // 0 means currently active / not yet parked.
    int64_t inactive_since_ms = 0;

    int offset_hz = 1500;   // where DX was heard
    // The offset this QSO transmits on, held from its first transmission to
    // its end, parked and reactivated included; 0 until one is rolled
    // (tx_offset.h, autoseq_hold_tx_offset()). A fresh entry starts on the
    // offset its station last heard from us (autoseq_remembered_tx_offset()).
    int tx_offset_hz = 0;
    int slot_id = 0;        // TX slot (0=even, 1=odd), in its own channel's mode's slots
    // An opening call whose station is working someone else (The busy hold): it transmits nothing and
    // spends no tries. held_since_ms is when the hold began (esp_timer ms), 0 while not held.
    bool held = false;
    int64_t held_since_ms = 0;
    // The channel it runs on, its dial and mode: a decode continues the entry for its call on its own channel
    // only, so an FT8 QSO and an FT4 QSO with one station are two entries. The CQ's is the one it was called on.
    RxChannel channel;
};

// TX entry for scheduling
struct AutoseqTxEntry {
    std::string text;       // Full FT8 message text
    std::string dxcall;     // Target callsign
    int offset_hz = 1500;
    int held_offset_hz = 0;  // QsoContext::tx_offset_hz: the QSO's held TX offset, 0 if none
    int slot_id = 0;
    int repeat_counter = AUTOSEQ_TRIES;  // tries left, this one included
    bool is_signoff = false; // True for TX4/TX5 (priority scheduling)
    uint16_t entry_id = 0;   // QsoContext::entry_id of the queue[0] this came from
    RxChannel channel;  // the channel it goes out on: its mode's slots and waveform, from its dial
};

// ADIF logging callback type
using AdifLogCallback = std::function<bool(const std::string& dxcall,
                                            const std::string& dxgrid,
                                            int rst_sent, int rst_rcvd, RxChannel channel)>;

// ============== Public API ==============
// Not locked: every call, and every callback, runs on the board's main loop
// (docs/architecture/main_board.md, P4). The decode task hands its decodes
// over rather than calling in (decode_pipeline_apply()).

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

// Call CQ on `channel`: queue the CQ entry on slot_parity (0 even, 1 odd), in
// that channel's mode's slots, with the station default's tries. It stays
// queued, below every QSO, until its tries are used or it is dropped. False if
// a CQ is already queued, on any channel (it is left as it is), or there is no
// room.
bool autoseq_call_cq(RxChannel channel, int slot_parity);

// The parity Call CQ on `channel` picks: an active QSO's on that channel if
// there is one, so the CQ keeps the period the station is already using there;
// otherwise the slot of that channel's mode that is under way when it can
// still be keyed, or the next one.
int autoseq_cq_parity(RxChannel channel, int current_slot_parity, bool current_slot_keyable);

// The queued CQ's channel; false if no CQ is queued.
bool autoseq_cq_channel(RxChannel* out);

// The queued CQ's entry_id, 0 if none.
uint16_t autoseq_cq_entry_id();

// Keep `offset_hz` as the TX offset of the active entry `entry_id`, for the
// rest of its QSO (tx_offset.h). False for an unknown entry or the CQ, which
// rolls every time.
bool autoseq_hold_tx_offset(uint16_t entry_id, int offset_hz);

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

// The busy hold: note what one decode, any decode, says of its sender (busy_hold.h), on its channel.
// Call for every decode of a slot, then autoseq_refresh_holds() once.
void autoseq_note_heard(const UiRxLine& msg);

// Hold each opening call whose station is busy, release the ones whose station is free, and end a hold
// older than AUTOSEQ_HOLD_MAX_MS; then re-sort, so the head is what can be answered.
void autoseq_refresh_holds();

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
// without its entry_id changing (a live change of modifier re-derives the CQ
// template for the entry already at CALLING) — a bare
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
// retries or re-emissions. `offset_hz` is the audio offset it goes out on, 0
// when nothing is keyed: remembered for its station, or as the CQ's, so a
// fresh entry for a station that heard it starts there (autoseq_remembered_tx_offset()).
void autoseq_on_tx_starting(int offset_hz = 0);

// The TX offset a fresh entry for `dxcall` on `channel` starts on (QsoContext::tx_offset_hz), 0 for none: the
// offset last transmitted to that station on that channel, within AUTOSEQ_INACTIVE_EXPIRY_MS; else, when the
// station is `calling_us`, the offset our CQ on that channel last went out on, within the same time
// (docs/architecture/autoseq.md, TX offset). Read at `now_ms`, esp_timer's clock.
int autoseq_remembered_tx_offset(const std::string& dxcall, RxChannel channel, bool calling_us, int64_t now_ms);

// Get display strings for active QSOs
void autoseq_get_qso_states(std::vector<std::string>& out);

// Copy the full active-zone QsoContext entries for structured consumers
// (e.g. the core_api). Returns active contexts only; inactive
// zone is not included. A copy, not a lock: main loop only, as above.
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

// "Worked this call recently?" -- true stops a decode from a caller with no
// queue entry (active or inactive) starting a new QSO, so a station just
// worked is not worked twice. Messages for an entry already in the queue are
// never filtered: a QSO is logged when we send RR73, so the station's 73 or a
// resent R+report arrives after it is "recent" and must still reach its
// entry. A touch is not filtered either. dxcall_norm is upper case with <>
// stripped; channel is the decode's, and a board answers for the band of its
// dial in its mode, as POTA counts a contact. Null (the default) filters nothing.
using AutoseqRecentQsoCallback = bool (*)(const std::string& dxcall_norm, RxChannel channel);
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

// The word between CQ and our call, "" for a plain CQ: "DX" gives
// "CQ DX K1ABC FN42". The caller passes one station_cq_mod_ok() accepts
// (components/station), since anything else is not a standard message.
void autoseq_set_cq_modifier(const std::string& modifier);
