#pragma once

// ============================================================================
// porta_proto.h — framing for the sidekick/main link (I28a, docs/architecture/sidekick.md (Port A)).
//
// Pure codec: no UART, no I2C, no globals, no ESP-IDF. Host-tested
// (host_test_porta_proto). It lives in components/ rather than main/ because
// two idf.py projects compile it -- the P4 and the sidekick -- which is
// prong 1 of docs/STYLE.md's module-placement rule.
//
// WIRE FORMAT
//
//     0xC6 | type | len | payload[len] | crc8
//
// `0xC6` is the existing companion sync byte and stays for a reason that is
// not aesthetic: main/porta.cpp decides whether a GPS or a sidekick is on
// Port A purely by listening, and 0xC6 was chosen because it cannot begin an
// NMEA sentence. Changing it would break role arbitration.
//
// The CRC covers type, len and payload -- NOT the sync byte. Sync is a
// constant, so it contributes no detection power, and excluding it means the
// checksummed bytes are exactly the ones that vary. That matters for B47: on a
// transport where framing is inherent (an I2C transaction has its own start
// and length) the sync byte is redundant and can be dropped without touching
// the CRC.
//
// WHY IT LOOKS LIKE THIS
//
// One message per frame, length-prefixed, max 255 bytes of payload. Frames
// stay small deliberately: docs/architecture/sidekick.md (Port A) requires framing that does not assume a
// stream, because B47 leaves UART-versus-I2C open, and small self-describing
// frames map onto a single I2C transaction as readily as onto a byte stream.
// A busy FT8 slot produces more decodes than fit in one frame; the polled
// design (also docs/architecture/sidekick.md (Port A)) handles that by the poller asking again, rather
// than by growing the frame.
//
// RESYNC, AND WHAT IT COSTS
//
// The decoder only looks for sync while idle. Once it has type and len it
// consumes exactly that many bytes, so a 0xC6 *inside* a payload is data and
// not a frame start -- which matters, because payload bytes are binary and
// 0xC6 will occur.
//
// A corrupt frame is dropped and the decoder returns to idle. It does not
// rescan the bytes it consumed. That is a deliberate simplification with a
// stated cost: if line noise happens to contain 0xC6 and is immediately
// followed by a real frame, that real frame is lost too. Recovery is the next
// frame -- at worst one poll interval, or one second for the beacon. Rescanning
// would recover it at the price of a buffer and a replay path, which is not
// worth it on a two-wire link between boards in the same enclosure.
// ============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORTA_PROTO_SYNC 0xC6u

// Bumped only when a change breaks an older peer. docs/architecture/sidekick.md (Installing the app bundle)'s version
// pairing applies here too: this is NOT the git build version the beacon
// carries -- they answer different questions, and conflating them is how a
// cosmetic rebuild reads as a protocol change.
// v2 (2026-09-28): DECODE carries its slot and the radio's dial frequency.
// v3 (2026-09-29): the FILE_READ QSO row carries my_pota_ref/my_sota_ref
// (architecture/logging.md); band now rides the row's own field too,
// unchanged in shape (still PORTA_BAND_MAX), so only the two new fields
// are a break.
// v4 (2026-09-30): SWR sweep (docs/architecture/radio.md, The SWR sweep): the SWEEP_START action and the SWEEP event. A v3 peer
// would answer the new action "unknown action" and ignore the event, so this is a break only in
// that the page's sweep cannot work against it; both ends are flashed together as for v3.
// v5 (2026-10-01): link stress test (RFC 0007 §3.11). A v4 peer logs LINK_TEST / LINK_DATA and
// does not run the test. Both ends are flashed together.
// v6 (2026-10-01): the link-test start frame carries the byte quota. A v5 peer
// ignores that start and does not run the test. Both ends are flashed together.
// v7 (2026-10-06): the FACTORY_RESET action (the sidekick's factory reset also resets the main
// board). A v6 host would NAK it as "unknown action", so a v7 sidekick refuses the reset rather
// than half doing it; both ends are flashed together as before.
// v8 (2026-10-14): the CQ is a queue entry with tries (docs/architecture/autoseq.md, Tries).
// BEACON goes; CQ_CALL, QUEUE_TRIES and QUEUE_PARITY come; QUEUE_ENTRY counts tries and carries
// the entry's parity and whether its count is its own; SLOT_STATE loses the beacon mode. Both
// ends are flashed together.
// v9 (2026-10-24): the RADIO_READ action (ask the radio for its state, IF;) and the TX HUD's
// not-keyed flag. An older host NAKs the action as unknown and ignores the flag. Both ends are
// flashed together.
// v10 (2026-10-24): the RADIO_STATE event, pushed when the radio reports a change (AI2;). An
// older sidekick ignores the event, so the chip only follows the radio when its panel is opened.
// Both ends are flashed together.
#define PORTA_PROTO_VERSION 10u

#define PORTA_PROTO_MAX_PAYLOAD 255u
#define PORTA_PROTO_MAX_FRAME (1u + 1u + 1u + PORTA_PROTO_MAX_PAYLOAD + 1u)  // 259

// Resting baud for both ends of Port A. The UART opens here; the link-test
// diagnostic may leave it and come back (porta_link_baud_now). 2500000 is the
// highest rate both main boards held for 14 MB (2026-10-01). 4000000 did not.
// Both ends flash together: a mixed pair never links.
#define PORTA_BAUD 2500000u

// v1 message types, derived by walking the host's existing UI surface rather
// than sketched: ~16 of its affordances are config values, ~13 are actions, and
// the rest are live views or paged bulk data. Three of these are *namespaces*
// addressed by an inner byte -- CONFIG by key, ACTION by verb, EVENT by subtype
// -- so new settings, verbs and telemetry are added inside a namespace and this
// list stops growing. That is why it is short and why it should stay short.
typedef enum {
    PORTA_MSG_HELLO        = 0x01,  // either end: protocol version + build version
    PORTA_MSG_POLL         = 0x02,  // sidekick -> host: anything pending?
    PORTA_MSG_NOTHING      = 0x03,  // host -> sidekick: nothing pending
    PORTA_MSG_DESCRIBE     = 0x04,  // sidekick -> host: describe yourself
    PORTA_MSG_MANIFEST     = 0x05,  // host -> sidekick: config metadata, actions, events
    PORTA_MSG_CONFIG_GET   = 0x06,
    PORTA_MSG_CONFIG_SET   = 0x07,
    PORTA_MSG_CONFIG_VALUE = 0x08,
    PORTA_MSG_ACTION       = 0x09,  // namespace: verb in the payload
    PORTA_MSG_EVENT        = 0x0A,  // namespace: subtype in the payload
    // FILE_LIST / FILE_READ (sidekick -> host) request one page (skip/take)
    // of day-file names or parsed QSO rows; the reply is zero or more
    // FILE_DATA frames -- one row each -- followed by ACK/NAK carrying the
    // request's own type as verb. See the FILE_* payloads section.
    PORTA_MSG_FILE_LIST    = 0x0B,
    PORTA_MSG_FILE_READ    = 0x0C,
    PORTA_MSG_FILE_DATA    = 0x0D,
    PORTA_MSG_ACK          = 0x0E,
    PORTA_MSG_NAK          = 0x0F,  // payload carries the reason
    // Firmware push toward the host (web app -> sidekick -> host). Separate
    // from the FILE_* messages on purpose: the failure semantics are different,
    // a 2 MB image takes minutes on this link, and a transfer that dies partway
    // must resume rather than restart.
    PORTA_MSG_FW_BEGIN     = 0x10,
    PORTA_MSG_FW_CHUNK     = 0x11,
    PORTA_MSG_FW_END       = 0x12,
    // Link stress test (RFC 0007 §3.11). Bulk, and both directions, so these
    // are their own types: ACTION is sidekick -> host only.
    PORTA_MSG_LINK_TEST    = 0x13,  // start / set-rate / stop / result
    PORTA_MSG_LINK_DATA    = 0x14,  // one max-length stress payload
} porta_msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[PORTA_PROTO_MAX_PAYLOAD];
} porta_frame_t;

// CRC-8, polynomial 0x07 (x^8 + x^2 + x + 1), init 0x00, no reflection.
// Bitwise rather than table-driven: 259 bytes at 1 Hz does not justify 256
// bytes of table on either part.
uint8_t porta_proto_crc8(const uint8_t *data, size_t len);

// Encodes one frame into `out`. Returns bytes written, or 0 if `out_cap` is
// too small -- never a partial frame, so a short buffer cannot put half a
// message on the wire.
size_t porta_proto_encode(uint8_t type, const uint8_t *payload, uint8_t len,
                          uint8_t *out, size_t out_cap);

// Byte-at-a-time decoder. Allocation-free, and identical for a UART stream and
// an I2C transaction replayed through it, which is what keeps the transport
// swappable (B47).
typedef struct {
    uint8_t  state;
    uint8_t  type;
    uint8_t  len;
    uint16_t got;      // payload bytes received so far
    uint8_t  payload[PORTA_PROTO_MAX_PAYLOAD];
    uint32_t frames_ok;
    uint32_t crc_errors;
} porta_decoder_t;

// Zeroes everything, counters included. Call this to start a new link.
void porta_decoder_init(porta_decoder_t *d);

// Back to hunting for sync, counters kept. A baud change leaves bytes in the
// UART that were clocked at the old rate; those must not finish a frame.
void porta_decoder_resync(porta_decoder_t *d);

// Feeds one byte. Returns true exactly when `out` has been filled with a
// complete, CRC-valid frame.
bool porta_decoder_push(porta_decoder_t *d, uint8_t byte, porta_frame_t *out);

// --- HELLO payload -------------------------------------------------------
// protocol_version (1 byte) + build_version (32 bytes, NUL-padded, NOT
// necessarily NUL-terminated -- a 32-character version fills the field).

#define PORTA_HELLO_VERSION_LEN 32u
#define PORTA_HELLO_PAYLOAD_LEN (1u + PORTA_HELLO_VERSION_LEN)

size_t porta_proto_encode_hello(uint8_t protocol_version, const char *build_version,
                                uint8_t *out, size_t out_cap);

// `build_version_out` must hold PORTA_HELLO_VERSION_LEN + 1 bytes; it is always
// NUL-terminated on success even when the wire field is not. False if the frame
// is not a well-formed HELLO.
bool porta_proto_parse_hello(const porta_frame_t *f, uint8_t *protocol_version_out,
                             char *build_version_out);

// --- EVENT payloads ------------------------------------------------------
// EVENT is a namespace, not a single message: the first payload byte is the
// subtype and the rest is subtype-specific. New telemetry is a new subtype, not
// a new frame type, so the type byte stops growing (docs/architecture/sidekick.md (Port A)).
//
// Every event carries a timestamp in the envelope rather than in each subtype's
// body, so a new subtype inherits one without thinking about it. It is the
// moment the event *happened*, stamped by the host at emit -- not the moment it
// arrived. Those differ: the outbound queue can hold events through a burst,
// and a viewer that stamped on arrival would compress a whole slot's decodes
// onto whatever second they finished draining.
//
// The sidekick has no clock of its own -- no RTC, no SNTP -- so the host is the
// only possible source. Zero means "the host does not know the time either",
// which is a real state on a cold unit with no GPS or DS3231, and must render
// as blank rather than as 1970.
//
// Text fields run to the end of the frame rather than being padded to a fixed
// width -- the frame already carries a length, so padding would only cost wire
// bytes to say something already known.

typedef enum {
    PORTA_EVT_LOG         = 0x01,  // one line of the host's on-screen log
    PORTA_EVT_DECODE      = 0x02,  // one decoded FT8/FT4 message
    PORTA_EVT_QUEUE_ENTRY = 0x03,  // one autoseq queue row: added, changed, or removed
    PORTA_EVT_SLOT_STATE  = 0x04,  // small scalars: slot parity, resolved TX offset
    PORTA_EVT_TX_HUD      = 0x05,  // TX HUD banner mirror: message, power/SWR, battery
    PORTA_EVT_USB_STATUS  = 0x06,  // host's own USB-C host port: a device attached or detached
    PORTA_EVT_SWEEP       = 0x07,  // SWR sweep progress: a point, a skipped band, done, aborted
    PORTA_EVT_RADIO_STATE = 0x08,  // the radio's own report of its state changed (IF; pushed by AI2;)
} porta_event_subtype_t;

#define PORTA_EVENT_TEXT_MAX 64  // matches RX_TEXT_MAX on the host

// `epoch_secs` is UTC seconds, or 0 when the host's clock is not set.
size_t porta_proto_encode_log(uint32_t epoch_secs, const char *text,
                              uint8_t *out, size_t out_cap);

// `text_out` must hold PORTA_EVENT_TEXT_MAX + 1 bytes. Always NUL-terminated
// on success. `epoch_secs_out` may be NULL. False if the frame is not a
// well-formed log event.
bool porta_proto_parse_log(const porta_frame_t *f, uint32_t *epoch_secs_out,
                           char *text_out);

typedef struct {
    uint32_t epoch_secs;   // UTC seconds, 0 when the host's clock is not set
    uint32_t decode_id;    // monotonic, assigned by the host at emit -- names this
                            // decode for a later QUEUE_REPLY action instead of making
                            // the browser re-parse rendered text back into a message
    char     text[PORTA_EVENT_TEXT_MAX + 1];
    int8_t   snr;          // FT8 reports span roughly -24..+50, so int8 is ample
    uint16_t offset_hz;    // 200..3000 in practice
    int16_t  dt_centis;    // time offset in hundredths of a second, signed
    bool     is_cq;
    bool     is_to_me;
    bool     is_recent_qso;
    uint32_t slot_idx;     // the UTC slot the audio was recorded in: it starts at
                           // slot_idx * slot_len_ds * 100 ms since the epoch; 0 when
                           // the host's clock is not set
    uint8_t  slot_len_ds;  // slot length in tenths of a second: 150 FT8, 75 FT4
    uint32_t dial_hz;      // the radio's dial frequency at the middle of this
                           // decode's signal; 0 when no tune is on record
} porta_decode_event_t;

size_t porta_proto_encode_decode(const porta_decode_event_t *d, uint8_t *out, size_t out_cap);
bool porta_proto_parse_decode(const porta_frame_t *f, porta_decode_event_t *out);

// --- QUEUE_ENTRY / SLOT_STATE payloads ------------------------------------
// Both are EVENT subtypes and share the subtype+epoch envelope, same as LOG
// and DECODE above.
//
// QUEUE_ENTRY reports one autoseq context. `state` is the wire form of
// AutoseqState (main/autoseq.h): 0=CALLING, 1=REPLYING, 2=REPORT,
// 3=ROGER_REPORT, 4=ROGERS, 5=SIGNOFF, 6=IDLE -- and IDLE *is* the removal
// signal, since that is already what the state means on the host (evicted by
// sort_and_clean). No separate "removed" flag is needed.
//
// `entry_id` is a stable identity assigned when the context is created, not
// a queue position: the queue re-sorts by priority (autoseq.cpp
// compare_ctx), so a browser snapshot's row index can point at a different
// context by the time a cancel arrives. QUEUE_CANCEL below addresses by this
// id for the same reason.
//
// AUTOSEQ_MAX_QUEUE is 30 (active + inactive), not a number that needs
// paging on its own -- the whole queue fits comfortably in individual
// per-change frames.
//
// `tries_sent` of `try_limit` are transmissions (docs/architecture/autoseq.md,
// Tries): a QSO's count is per step, the CQ's (dxcall "CQ") is the whole CQ.

#define PORTA_CALLSIGN_MAX 16u

#define PORTA_QUEUE_STATE_IDLE 6u  // AutoseqState::IDLE's wire value: the entry is gone

#define PORTA_QUEUE_ODD    0x01u  // the entry transmits on odd slots
#define PORTA_QUEUE_CUSTOM 0x02u  // its count is its own, not the station default

typedef struct {
    uint32_t epoch_secs;
    uint16_t entry_id;
    uint8_t  state;         // AutoseqState wire value
    uint8_t  tries_sent;
    uint8_t  try_limit;
    uint8_t  flags;         // PORTA_QUEUE_*
    char     dxcall[PORTA_CALLSIGN_MAX + 1];
} porta_queue_entry_event_t;

size_t porta_proto_encode_queue_entry(const porta_queue_entry_event_t *q,
                                      uint8_t *out, size_t out_cap);
bool porta_proto_parse_queue_entry(const porta_frame_t *f, porta_queue_entry_event_t *out);

// SLOT_STATE is global scalars, not any one queue entry's: `slot_parity` is
// the armed TX's slot parity (0=even, 1=odd), and `resolved_offset_hz` is the
// offset that armed TX resolved to, 0 when nothing is armed.
typedef struct {
    uint32_t epoch_secs;
    uint8_t  slot_parity;
    uint16_t resolved_offset_hz;
} porta_slot_state_event_t;

size_t porta_proto_encode_slot_state(const porta_slot_state_event_t *s,
                                     uint8_t *out, size_t out_cap);
bool porta_proto_parse_slot_state(const porta_frame_t *f, porta_slot_state_event_t *out);

// USB_STATUS: the host's own USB-C host port (radio normally, briefly the
// sidekick during its own USB-C flash workflow) attached or detached a
// device. Pushed the instant usb_c_presence_host.cpp's event fires -- not
// polled, not inferred from cat_ready/audio_rx going stale -- so the browser
// can react immediately and say specifically what happened, rather than a
// fragile string match on the on-screen toast's text (that text is for a
// human reading /log; this is the typed signal the UI decides on).
// `kind` is UsbCKind's own wire value (0=QmxQdx, 1=EspressifRom, 2=Other) --
// the same classification usb_c_presence.h already computes for the
// on-device toast, just carried onto the wire instead of staying local.
// vid/pid are always included, even for a recognized kind, so a future
// unrecognized device is never silently unidentifiable.
typedef struct {
    uint32_t epoch_secs;
    bool     present;  // true = attach, false = detach
    uint8_t  kind;      // UsbCKind wire value
    uint16_t vid;
    uint16_t pid;
} porta_usb_status_event_t;

size_t porta_proto_encode_usb_status(const porta_usb_status_event_t *u,
                                     uint8_t *out, size_t out_cap);
bool porta_proto_parse_usb_status(const porta_frame_t *f, porta_usb_status_event_t *out);

// RADIO_STATE: what the radio last said of itself, sent when it changes (a knob turn, a mode or
// split change, TX starting or ending). The fields are IF;'s: `tx` 1 = transmitting, `mode` is
// the MD digit, `split` 1 = on.
typedef struct {
    uint32_t epoch_secs;
    uint32_t freq_hz;
    uint8_t  tx;
    uint8_t  mode;
    uint8_t  split;
} porta_radio_state_event_t;

size_t porta_proto_encode_radio_state(const porta_radio_state_event_t *r,
                                      uint8_t *out, size_t out_cap);
bool porta_proto_parse_radio_state(const porta_frame_t *f, porta_radio_state_event_t *out);

// SWEEP (docs/architecture/radio.md, The SWR sweep). One event per measured point, plus one each for a band the radio does not
// have, the end of a sweep, and an abort. `kind` and `reason` are wire values, below.
typedef enum {
    PORTA_SWEEP_POINT = 0,         // freq_hz, swr_c
    PORTA_SWEEP_BAND_SKIPPED = 1,  // range_index: the radio refused both edges
    PORTA_SWEEP_DONE = 2,
    PORTA_SWEEP_ABORTED = 3,       // reason
} porta_sweep_kind_t;

typedef enum {
    PORTA_SWEEP_REASON_CANCELLED = 0,
    PORTA_SWEEP_REASON_CAT_LOST = 1,
    PORTA_SWEEP_REASON_STALLED = 2,
} porta_sweep_reason_t;

typedef struct {
    uint32_t epoch_secs;
    uint8_t  kind;         // porta_sweep_kind_t
    uint8_t  range_index;  // which requested range, in request order
    uint32_t freq_hz;      // POINT
    int16_t  swr_c;        // POINT: SWR x 100, -1 otherwise
    uint8_t  reason;       // ABORTED: porta_sweep_reason_t
} porta_sweep_event_t;

size_t porta_proto_encode_sweep_event(const porta_sweep_event_t *e, uint8_t *out, size_t out_cap);
bool porta_proto_parse_sweep_event(const porta_frame_t *f, porta_sweep_event_t *out);

// The SWEEP_START action: up to PORTA_SWEEP_MAX_RANGES (lo, hi, step) ranges in kHz. The host
// validates them (swr_sweep.h); this only frames them.
#define PORTA_SWEEP_MAX_RANGES 12u

typedef struct {
    uint16_t lo_khz;
    uint16_t hi_khz;
    uint16_t step_khz;
} porta_sweep_range_t;

size_t porta_proto_encode_sweep_start(const porta_sweep_range_t *ranges, size_t count,
                                      uint8_t *out, size_t out_cap);
// `ranges_out` holds PORTA_SWEEP_MAX_RANGES. False if malformed or empty.
bool porta_proto_parse_sweep_start(const porta_frame_t *f, porta_sweep_range_t *ranges_out,
                                   size_t *count_out);

// TX_HUD mirrors components/ui/include/tx_hud_banner.h's TxHudBannerInput --
// same raw fields the on-device banner already computes each ~500 ms while
// visible, not a re-derivation. power_w/swr travel as fixed-point (x10/x100)
// since the wire has no float; -1 means "not read yet" on every numeric
// field, the same sentinel TxHudBannerInput itself uses. `active` is the
// TX_HUD_LEN-carried banner-visible flag (tx_hud_visible(): TX running, or
// the post-abort linger window) -- the browser hides its panel the instant
// an event arrives with this false, rather than guessing from a timeout.
// offset_hz is not part of TxHudBannerInput (the on-screen banner has no
// reason to show it -- the operator can see the cursor on STATUS) but is the
// one genuinely new field here: g_pending_tx.offset_hz, the same value
// log_rxtx_line() already treats as "what's actually going on air" -- unlike
// SLOT_STATE's resolved_offset_hz, which is the armed TX's, before it goes
// on the air. 0 = unknown,
// same convention as SLOT_STATE's own offset field.
#define PORTA_TX_HUD_TEXT_MAX PORTA_EVENT_TEXT_MAX
#define PORTA_TX_HUD_REASON_MAX 24u

typedef struct {
    uint32_t epoch_secs;
    bool     active;
    bool     aborted;
    bool     writes_blocked;
    bool     not_keyed;     // active with nothing on the air: the slot was skipped and not counted; `reason` says why
    int16_t  power_dw;      // deciwatts (power_w * 10), -1 = unknown
    int16_t  swr_c;         // SWR * 100, -1 = unknown
    int8_t   battery_pct;   // -1 = unknown
    uint16_t offset_hz;     // what's actually being transmitted at, 0 = unknown
    char     reason[PORTA_TX_HUD_REASON_MAX + 1];  // why `aborted`, e.g. "low battery";
                                                    // empty when not aborted -- sourced from
                                                    // the actual abort site, not guessed
                                                    // client-side, so a future second abort
                                                    // reason shows up as itself
    char     text[PORTA_TX_HUD_TEXT_MAX + 1];
} porta_tx_hud_event_t;

size_t porta_proto_encode_tx_hud(const porta_tx_hud_event_t *h, uint8_t *out, size_t out_cap);
bool porta_proto_parse_tx_hud(const porta_frame_t *f, porta_tx_hud_event_t *out);

// --- ACTION payloads -----------------------------------------------------
// ACTION is the other namespace: payload[0] is the verb, the rest is
// verb-specific. Actions travel sidekick -> host and are answered with ACK or
// NAK carrying the same verb, so a reply can be matched to its request without
// a sequence number -- adequate while one action is outstanding at a time,
// which is all the polled design permits.

typedef enum {
    PORTA_ACT_SET_CLOCK = 0x01,  // epoch seconds + milliseconds
    // 0x02 was TX_FREE (one-shot free-text); removed I33 — do not reuse without
    // a flag day (old sidekicks still encode it).
    PORTA_ACT_TX_CANCEL = 0x03,  // abort in-flight / clear armed TX
    PORTA_ACT_CONNECT   = 0x04,  // start UAC + CAT sync (STATUS key 2)
    PORTA_ACT_TUNE      = 0x05,  // payload: u8 on (STATUS key 4)
    // 0x06 was BEACON (Off/Even/Odd); removed in v8 for CQ_CALL. Do not reuse
    // without a flag day.
    PORTA_ACT_QUEUE_CANCEL = 0x07,  // payload: u16 entry_id (not a list index)
    PORTA_ACT_QUEUE_REPLY  = 0x08,  // payload: u32 decode_id
    PORTA_ACT_FILE_DELETE  = 0x09,  // payload: filename (empty = clear all daily logs)
    PORTA_ACT_SWEEP_START  = 0x0A,  // payload: u8 count, then count x (lo, hi, step) kHz, each u16 LE
    PORTA_ACT_FACTORY_RESET = 0x0B, // payload: u8 flags (PORTA_FACTORY_RESET_LOGS)
    PORTA_ACT_CQ_CALL      = 0x0C,  // verb only: queue a CQ; the host picks its parity
    PORTA_ACT_QUEUE_TRIES  = 0x0D,  // payload: u16 entry_id, u8 tries (0 = station default)
    PORTA_ACT_QUEUE_PARITY = 0x0E,  // payload: u16 entry_id, u8 parity (the CQ only)
    PORTA_ACT_RADIO_READ   = 0x0F,  // verb only: ask the radio for its state (IF;); the answer is in the config snapshot
} porta_action_verb_t;

// FACTORY_RESET flags. Station settings are always reset; the QSO logs only when asked,
// because they cannot be recreated.
#define PORTA_FACTORY_RESET_LOGS 0x01u

// The browser is the clock source in a headless build: it is the only device
// present that reliably knows the time, and unlike NTP it knows it without
// internet -- which is the case that matters, since a cold radio on a summit
// will not decode until UTC is right.
size_t porta_proto_encode_set_clock(uint32_t epoch_secs, uint16_t millis,
                                    uint8_t *out, size_t out_cap);
bool porta_proto_parse_set_clock(const porta_frame_t *f, uint32_t *epoch_secs_out,
                                 uint16_t *millis_out);

// Cancel: verb only.
size_t porta_proto_encode_tx_cancel(uint8_t *out, size_t out_cap);
bool porta_proto_parse_tx_cancel(const porta_frame_t *f);

// Connect: verb only — same as STATUS → 2 (start RX audio + CAT sync).
size_t porta_proto_encode_connect(uint8_t *out, size_t out_cap);
bool porta_proto_parse_connect(const porta_frame_t *f);

// Tune: verb + u8 on (1 = TX tone, 0 = RX).
size_t porta_proto_encode_tune(bool on, uint8_t *out, size_t out_cap);
bool porta_proto_parse_tune(const porta_frame_t *f, bool *on_out);

// Call CQ: verb only. The host picks the parity (an active QSO's, or the slot
// it can still key) and the tries (the station default), because it holds the
// slot clock and the setting.
size_t porta_proto_encode_cq_call(uint8_t *out, size_t out_cap);
bool porta_proto_parse_cq_call(const porta_frame_t *f);
size_t porta_proto_encode_radio_read(uint8_t *out, size_t out_cap);
bool porta_proto_parse_radio_read(const porta_frame_t *f);

// Queue tries: verb + u16 entry_id + u8 tries. 0 returns the entry to the
// station default; the host refuses a count out of range for that entry.
size_t porta_proto_encode_queue_tries(uint16_t entry_id, uint8_t tries,
                                      uint8_t *out, size_t out_cap);
bool porta_proto_parse_queue_tries(const porta_frame_t *f, uint16_t *entry_id_out,
                                   uint8_t *tries_out);

// Queue parity: verb + u16 entry_id + u8 parity (0 even, 1 odd). Only the CQ
// moves; a QSO's parity is set by the station it works.
size_t porta_proto_encode_queue_parity(uint16_t entry_id, uint8_t parity,
                                       uint8_t *out, size_t out_cap);
bool porta_proto_parse_queue_parity(const porta_frame_t *f, uint16_t *entry_id_out,
                                    uint8_t *parity_out);

// Queue cancel: verb + u16 entry_id. See porta_queue_entry_event_t above for
// why this is an id and not a queue position.
size_t porta_proto_encode_queue_cancel(uint16_t entry_id, uint8_t *out, size_t out_cap);
bool porta_proto_parse_queue_cancel(const porta_frame_t *f, uint16_t *entry_id_out);

// Queue reply: verb + u32 decode_id, naming a decode the browser was shown
// rather than re-sending its text.
size_t porta_proto_encode_queue_reply(uint32_t decode_id, uint8_t *out, size_t out_cap);
bool porta_proto_parse_queue_reply(const porta_frame_t *f, uint32_t *decode_id_out);

// Factory reset: verb + u8 flags (PORTA_FACTORY_RESET_*). The host resets its Station settings
// to their defaults without restarting (a P4 reset can leave the QMX unenumerated, row qdcm), and
// deletes the daily QSO logs too when the LOGS flag is set. ACK once done, NAK with a reason when
// it refused (transmitting) or failed.
size_t porta_proto_encode_factory_reset(uint8_t flags, uint8_t *out, size_t out_cap);
bool porta_proto_parse_factory_reset(const porta_frame_t *f, uint8_t *flags_out);

// File delete: verb + basename (0..PORTA_FILENAME_MAX). Empty = clear all
// daily QSO logs (FILE_LIST set / phone "Clear all ADI files"). Active-day
// LOCK is host policy. `filename_out` needs PORTA_FILENAME_MAX + 1 bytes.
size_t porta_proto_encode_file_delete(const char *filename, uint8_t *out, size_t out_cap);
bool porta_proto_parse_file_delete(const porta_frame_t *f, char *filename_out);

size_t porta_proto_encode_ack(uint8_t verb, uint8_t *out, size_t out_cap);

// `reason` is short free text for a human; it reaches the operator's log, so it
// should say what was wrong rather than name a code.
size_t porta_proto_encode_nak(uint8_t verb, const char *reason,
                              uint8_t *out, size_t out_cap);

bool porta_proto_parse_ack(const porta_frame_t *f, uint8_t *verb_out);

// `reason_out` must hold PORTA_EVENT_TEXT_MAX + 1 bytes.
bool porta_proto_parse_nak(const porta_frame_t *f, uint8_t *verb_out, char *reason_out);

// --- CONFIG payloads -----------------------------------------------------
// CONFIG_* share one shape: key_len (1) + key[key_len] + value_bytes…
// CONFIG_GET with key_len == 0 means "all keys" (Station.txt surface).
// CONFIG_SET / CONFIG_VALUE require a non-empty key. Values are the same
// text Station.txt would store. ACK/NAK for SET (and end-of-GET-all) reuse
// PORTA_MSG_ACK/NAK with verb = the CONFIG message type (0x06 / 0x07).

#define PORTA_CONFIG_KEY_MAX 32u
#define PORTA_CONFIG_VALUE_MAX 200u

size_t porta_proto_encode_config_get(const char *key, uint8_t *out, size_t out_cap);
size_t porta_proto_encode_config_set(const char *key, const char *value,
                                     uint8_t *out, size_t out_cap);
size_t porta_proto_encode_config_value(const char *key, const char *value,
                                       uint8_t *out, size_t out_cap);

// `key_out` / `value_out` must hold KEY_MAX+1 / VALUE_MAX+1. Empty key on
// GET means get-all. SET/VALUE parsers require a non-empty key.
bool porta_proto_parse_config_get(const porta_frame_t *f, char *key_out);
bool porta_proto_parse_config_set(const porta_frame_t *f, char *key_out, char *value_out);
bool porta_proto_parse_config_value(const porta_frame_t *f, char *key_out, char *value_out);

// --- FILE_LIST / FILE_READ / FILE_DATA payloads ---------------------------
// FILE_LIST and FILE_READ (sidekick -> host) request one page -- skip/take,
// the same shape components/qso_log/include/qso_browse.h's QsoBrowsePager already uses --
// reusing that pager instead of inventing wire-level pagination a second
// time. The reply is zero or more FILE_DATA frames, one row each (so a short
// buffer can never leave a row half-written), followed by ACK/NAK carrying
// the request's own message type as verb -- the same "burst then ACK" shape
// CONFIG_GET-all already established. Fewer rows than `take` arriving before
// the ACK means end of list/file; the caller pages further with a higher
// `skip`, exactly like the pager's own `has_next`.
//
// FILE_READ ships *parsed* QSO rows (QsoLogEntry), never raw ADIF bytes --
// ADIF is verbose text and the parser already exists and is host-tested
// (qso_browse_pager_feed); re-parsing it in JavaScript would cost wire
// budget and correctness for nothing.
//
// mode/station_callsign (added for the browser's "Download ADI" button)
// exist for the same reason as every other field here: the browser
// reconstructs a real per-record ADIF line client-side from these rows, and
// both are required ADIF fields the record can't omit -- unlike grid/freq/
// comment, which are genuinely optional per QSO.

typedef enum {
    PORTA_FILE_LIST_QSO_DAILY = 0x01,  // matches FileListKind::QsoDaily
} porta_file_list_kind_t;

typedef enum {
    PORTA_FILE_DATA_NAME  = 0x01,  // one FILE_LIST row: a day-file name
    PORTA_FILE_DATA_ENTRY = 0x02,  // one FILE_READ row: one parsed QSO
} porta_file_data_kind_t;

#define PORTA_FILENAME_MAX 32u
#define PORTA_BAND_MAX 8u
#define PORTA_GRID_MAX 8u
#define PORTA_FREQ_MAX 12u
#define PORTA_COMMENT_MAX 48u
#define PORTA_MODE_MAX 7u  // "FT8"/"FT4" today; room for a longer protocol name
// Comma lists, no program prefix (architecture/logging.md) -- room for a
// handful of references (e.g. "US-1234,US-5678,US-9012" is 23 characters);
// SOTA is realistically always one summit, but sized the same for
// simplicity.
#define PORTA_POTA_REF_MAX 40u
#define PORTA_SOTA_REF_MAX 40u

size_t porta_proto_encode_file_list_req(uint8_t kind, uint16_t skip, uint8_t take,
                                        uint8_t *out, size_t out_cap);
bool porta_proto_parse_file_list_req(const porta_frame_t *f, uint8_t *kind_out,
                                     uint16_t *skip_out, uint8_t *take_out);

// `filename_out` must hold PORTA_FILENAME_MAX + 1 bytes.
size_t porta_proto_encode_file_read_req(const char *filename, uint16_t skip, uint8_t take,
                                        uint8_t *out, size_t out_cap);
bool porta_proto_parse_file_read_req(const porta_frame_t *f, char *filename_out,
                                     uint16_t *skip_out, uint8_t *take_out);

// `name_out` must hold PORTA_FILENAME_MAX + 1 bytes.
size_t porta_proto_encode_file_name_row(const char *name, uint8_t *out, size_t out_cap);
bool porta_proto_parse_file_name_row(const porta_frame_t *f, char *name_out);

// time_on is "HH:MM" (qso_browse.cpp's own width -- "??:??" when unknown).
// rst_sent/rst_rcvd use -99 for "no report", the same sentinel
// QsoContext::snr_tx/snr_rx already use, rather than a separate has-flag.
typedef struct {
    char    time_on[6];
    char    band[PORTA_BAND_MAX + 1];
    char    call[PORTA_CALLSIGN_MAX + 1];
    int8_t  rst_sent;
    int8_t  rst_rcvd;
    char    grid[PORTA_GRID_MAX + 1];        // their grid (<gridsquare>), empty if absent
    char    freq[PORTA_FREQ_MAX + 1];        // raw ADIF MHz string, empty if absent
    char    my_grid[PORTA_GRID_MAX + 1];     // own grid at the time (<my_gridsquare>), empty if absent
    char    comment[PORTA_COMMENT_MAX + 1];  // <comment>, empty if absent
    char    mode[PORTA_MODE_MAX + 1];              // <mode>, e.g. "FT8"
    char    station_callsign[PORTA_CALLSIGN_MAX + 1]; // <station_callsign>, own call at log time
    char    my_pota_ref[PORTA_POTA_REF_MAX + 1];   // <my_pota_ref>, empty if absent
    char    my_sota_ref[PORTA_SOTA_REF_MAX + 1];   // <my_sota_ref>, empty if absent
} porta_qso_entry_row_t;

size_t porta_proto_encode_file_entry_row(const porta_qso_entry_row_t *e,
                                         uint8_t *out, size_t out_cap);
bool porta_proto_parse_file_entry_row(const porta_frame_t *f, porta_qso_entry_row_t *out);

// --- Link stress test (RFC 0007 §3.11) ------------------------------------
// One direction's payloads. The sequence starts at 0 when that direction
// starts; both directions share the seed from LINK_TEST start and keep
// their own sequences. Pattern is seq % 5, in this order: 0xC6, 0x00, 0xFF,
// alternating 0xAA/0x55, then a per-byte mix of (seed, seq, index) so a gap
// does not desync the checker. Bytes 0..3 are the sequence, little-endian;
// the checker regenerates the whole payload and compares every byte.
// CRC failures stay on porta_decoder_t. This checker only sees a frame the
// decoder already accepted, and counts a drop, a duplicate, or a mismatch.

typedef enum {
    PORTA_LINK_PAT_SYNC = 0,
    PORTA_LINK_PAT_ZERO = 1,
    PORTA_LINK_PAT_ONES = 2,
    PORTA_LINK_PAT_ALT  = 3,
    PORTA_LINK_PAT_PRNG = 4,
    PORTA_LINK_PAT_COUNT = 5,
} porta_link_pattern_t;

typedef enum {
    PORTA_LINK_START    = 1,  // + u32 seed + u8 tune + u32 pass_bytes
    PORTA_LINK_SET_RATE = 2,  // + u32 baud, one of porta_link_rate_at()
    PORTA_LINK_STOP     = 3,
    PORTA_LINK_RESULT   = 4,  // this end's counters for the rate in progress
} porta_link_cmd_t;

// No LINK_DATA for this long and the rate falls back to PORTA_BAUD.
// Also how long a sender waits after its own quota for the other direction.
#define PORTA_LINK_IDLE_MS 3000u

// Payload bytes one direction must exchange, clean, before that rate passes.
// This is the final pass: about 10x the largest image (RFC 0007 §3.11).
// Settings may start a shorter run; the quota has to be one porta_link_pass_at()
// returns, and the start frame carries it so both ends use the same number.
#define PORTA_LINK_PASS_BYTES 14000000u

// After the set-rate frame has left the wire, wait this long before the next
// data frame. The peer may still be shifting out a full TX ring (4096 bytes,
// about 360 ms at 115200) before it can change baud.
#define PORTA_LINK_SETTLE_MS 800u

typedef struct {
    uint32_t seed;
    uint32_t next_seq;
    uint32_t frames_ok;
    uint32_t drops;
    uint32_t duplicates;
    uint32_t mismatches;
} porta_link_checker_t;

void porta_link_data_fill(uint8_t payload[PORTA_PROTO_MAX_PAYLOAD],
                          uint32_t seed, uint32_t seq);
void porta_link_checker_init(porta_link_checker_t *c, uint32_t seed);
// False if `payload` is not one max-length stress payload. Counters unchanged.
bool porta_link_checker_push(porta_link_checker_t *c, const uint8_t *payload,
                             uint8_t len);

// Index 0 is PORTA_BAUD. The other entries are the rates the diagnostic can
// switch to. A baud not in the list is refused by encode and by parse.
unsigned porta_link_rate_count(void);
uint32_t porta_link_rate_at(unsigned index);
bool porta_link_rate_allowed(uint32_t baud);

// Quotas Settings may ask for, shortest first. The last is PORTA_LINK_PASS_BYTES.
unsigned porta_link_pass_count(void);
uint32_t porta_link_pass_at(unsigned index);
bool porta_link_pass_allowed(uint32_t bytes);

// Baud to run at `now_ms`. `last_data_ms` is the last LINK_DATA on this
// rate (set it to `now_ms` when the rate is applied). An idle gap of
// PORTA_LINK_IDLE_MS, or a baud that is not allowed, returns PORTA_BAUD.
uint32_t porta_link_baud_now(uint32_t requested_baud, uint32_t now_ms,
                            uint32_t last_data_ms);

size_t porta_proto_encode_link_start(uint32_t seed, bool tune, uint32_t pass_bytes,
                                     uint8_t *out, size_t out_cap);
size_t porta_proto_encode_link_set_rate(uint32_t baud, uint8_t *out, size_t out_cap);
size_t porta_proto_encode_link_stop(uint8_t *out, size_t out_cap);
size_t porta_proto_encode_link_data(uint32_t seed, uint32_t seq,
                                   uint8_t *out, size_t out_cap);
bool porta_proto_parse_link_start(const porta_frame_t *f, uint32_t *seed_out,
                                  bool *tune_out, uint32_t *pass_bytes_out);
bool porta_proto_parse_link_set_rate(const porta_frame_t *f, uint32_t *baud_out);
bool porta_proto_parse_link_stop(const porta_frame_t *f);

// One end's view of the rate in progress. crc_errors is the delta since the
// rate started, not the decoder's lifetime total.
typedef struct {
    uint32_t baud;
    uint32_t frames_ok;
    uint32_t drops;
    uint32_t duplicates;
    uint32_t mismatches;
    uint32_t crc_errors;
    uint32_t tx_bytes;
    bool running;
    bool failed;
    bool passed;
    bool tune;
} porta_link_status_t;

size_t porta_proto_encode_link_result(const porta_link_status_t *status,
                                     uint8_t *out, size_t out_cap);
bool porta_proto_parse_link_result(const porta_frame_t *f, porta_link_status_t *out);

typedef enum {
    PORTA_LINK_PHASE_IDLE = 0,
    PORTA_LINK_PHASE_RUN = 1,
    PORTA_LINK_PHASE_SETTLE = 2,
    PORTA_LINK_PHASE_DONE = 3,
} porta_link_phase_t;

// Coordinator (the sidekick) sends start, advances the rate, and sends stop.
// The peer (a main board) starts when it sees start, and follows set-rate.
typedef struct {
    porta_link_phase_t phase;
    bool coordinator;
    bool tune;
    bool hold_tx;
    bool stop_requested;
    bool stop_sent;
    bool need_start;
    // Coordinator only. After START, switch once to rate_last before any
    // quota. The run does not visit the rates in between.
    bool need_rate;
    bool rate_failed;
    bool sweep_passed;
    bool note_ready;
    bool crc_latched;
    uint32_t seed;
    uint32_t pass_bytes;
    uint32_t tx_seq;
    uint32_t tx_bytes;
    uint32_t baud;
    uint32_t pending_baud;
    uint32_t settle_at_ms;
    uint32_t last_data_ms;
    uint32_t last_rx_ms;
    uint32_t last_result_ms;
    uint32_t crc_base;
    unsigned rate_index;
    unsigned rate_last;  // the one rate to run, as an index; the peer does not use it
    porta_link_checker_t rx;
    porta_link_status_t peer;
    bool peer_valid;
    // After a rate switch the peer stays quiet until the coordinator's first
    // frame on the new rate, so the two settles cannot eat a sequence number.
    bool await_coord;
    char note[PORTA_EVENT_TEXT_MAX + 1];
} porta_link_run_t;

void porta_link_run_init(porta_link_run_t *r);
void porta_link_run_begin(porta_link_run_t *r, uint32_t seed, uint32_t pass_bytes,
                          bool coordinator, bool tune, unsigned last_rate,
                          uint32_t now_ms);
void porta_link_run_stop(porta_link_run_t *r);
bool porta_link_run_active(const porta_link_run_t *r);

// `apply_baud` is set when this frame must leave the UART before the baud
// changes (a set-rate). Zero means no change. Call porta_link_run_applied
// after that frame has been flushed and the UART is on `apply_baud`.
size_t porta_link_run_next(porta_link_run_t *r, uint32_t now_ms, uint32_t crc_errors,
                           uint8_t *out, size_t out_cap, uint32_t *apply_baud);

// `switch_baud` is set when the peer must change baud before reading further.
void porta_link_run_on_frame(porta_link_run_t *r, const porta_frame_t *f,
                             uint32_t now_ms, uint32_t crc_errors, uint32_t *switch_baud);
void porta_link_run_applied(porta_link_run_t *r, uint32_t now_ms);
void porta_link_run_status(const porta_link_run_t *r, uint32_t crc_errors,
                           porta_link_status_t *out);
// A one-line note when a rate finishes, or 0 if nothing new. Fits the log.
size_t porta_link_run_take_note(porta_link_run_t *r, char *out, size_t cap);

// Browser event rows. The sidekick's GET /api/events and the host scenario
// simulator both call these, so the JSON the page polls has one encoder.
// Each writes one JSON object, no leading comma. Returns the bytes written,
// excluding the NUL, or 0 if `out` is too small. `seq` is the ring cursor
// the poll's `since` compares against.
size_t porta_event_json_log(char *out, size_t cap, uint32_t seq,
                            uint32_t epoch_secs, const char *text);
size_t porta_event_json_decode(char *out, size_t cap, uint32_t seq,
                               const porta_decode_event_t *ev);
size_t porta_event_json_queue(char *out, size_t cap, uint32_t seq,
                              const porta_queue_entry_event_t *ev);
size_t porta_event_json_slot(char *out, size_t cap, uint32_t seq,
                             const porta_slot_state_event_t *ev);
size_t porta_event_json_tx_hud(char *out, size_t cap, uint32_t seq,
                               const porta_tx_hud_event_t *ev);
size_t porta_event_json_usb(char *out, size_t cap, uint32_t seq,
                            const porta_usb_status_event_t *ev);
size_t porta_event_json_sweep(char *out, size_t cap, uint32_t seq,
                              const porta_sweep_event_t *ev);
size_t porta_event_json_radio(char *out, size_t cap, uint32_t seq,
                              const porta_radio_state_event_t *ev);

#ifdef __cplusplus
}
#endif
