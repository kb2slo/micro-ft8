#pragma once

#include <stdint.h>

#include "porta_proto.h"

// ============================================================================
// porta.h — the sidekick link on Grove G1/G2 (UART1).
//
// This port used to be shared. A GPS puck and the sidekick were either/or on
// the same two pins with no presence detection, so the module listened to the
// byte stream and guessed: NMEA sentences meant GPS, a 0xC6-prefixed frame
// meant the sidekick, and it probed two baud rates until one of them produced
// a checksum-valid frame. All of that is gone. Grove GPS was dropped in favour
// of the GNSS/LoRa cap's own receiver, so the port has exactly one possible
// occupant and needs no arbitration, no role, and no baud probe.
//
// The reason it had to go is not tidiness. Choosing the cap as the GPS source
// meant porta_start() was never called at all, so the sidekick link simply did
// not exist -- and in a headless build (I28) that would have removed the only
// UI, along with the only control capable of putting it back.
//
// What the link carries now: docs/architecture/sidekick.md (Port A: the link
// to the main board).
// ============================================================================

// Brings up UART1 on G1/G2 at PORTA_BAUD (porta_proto.h) and starts listening
// for beacons. A no-op if already running.
void porta_start();

// Releases the UART.
void porta_stop();

// Periodic pump — call every main-loop tick. Drains the outbound queue as well
// as reading beacons.
void porta_tick();

// True while a link stress test is exchanging frames. Transmit actions are
// refused for the duration; the test's own Tune pass is not one of them.
bool porta_link_test_active();

// ---------------------------------------------------------------------------
// Outbound events (I28a). Queued, never sent inline: the callers are the slot
// loop and the log path, and neither can afford to wait on a UART. The queue is
// bounded and **drops the oldest** when full rather than blocking or growing --
// a lost log line costs nothing, a missed TX window costs a QSO.
//
// Silently a no-op until porta_start() has run, so callers need no guard.
// ---------------------------------------------------------------------------

// One line of the on-screen log.
void porta_emit_log(const char* text);

// One decoded message. `dt_s` is the decoder's time offset in seconds.
// `decode_id` names this decode for a later PORTA_ACT_QUEUE_REPLY -- assigned
// by the caller (main.cpp's remember_decode_for_reply()), not here, since
// only the caller has anywhere to resolve it back to a message.
// slot_idx/slot_len_ds: the UTC slot the audio was recorded in; dial_hz: the
// radio's dial at the middle of this decode's signal, 0 if unknown (Port A v2).
void porta_emit_decode(uint32_t decode_id, const char* text, int snr, int offset_hz,
                       float dt_s, bool is_cq, bool is_to_me, bool is_recent_qso,
                       uint32_t slot_idx, uint8_t slot_len_ds, uint32_t dial_hz);

// One autoseq queue row (I28d, docs/architecture/webapp.md (Screens)). `state` is the AutoseqState
// wire value; IDLE (6) is the removal signal, not a separate flag -- callers
// use it to tell a browser an entry is gone, same as the host side does.
void porta_emit_queue_entry(uint16_t entry_id, uint8_t state, uint8_t retry_count,
                            uint8_t retry_limit, const char* dxcall);

// Global slot scalars, not any one queue entry's -- see porta_slot_state_event_t
// in porta_proto.h. Emitted periodically (main.cpp), not on change: one
// mechanism serves both "keep the browser current" and "give a freshly
// loaded browser today's state" without a separate getter or diff logic.
void porta_emit_slot_state(uint8_t slot_parity, uint8_t beacon_mode,
                           uint16_t resolved_offset_hz);

// Mirrors the on-device TX HUD banner (tx_hud_banner.h's TxHudBannerInput):
// same raw fields, emitted periodically while the banner is visible
// (main.cpp's draw_tx_hud() already computes these each ~500 ms). `active`
// false is the "hide the panel" signal -- one final call at that transition,
// not a timeout the browser has to guess. power_w/swr are -1 for "not read
// yet"; battery_pct is -1 if unavailable. offset_hz is what's actually going
// on air (g_pending_tx.offset_hz), 0 if there is no pending TX -- distinct
// from SLOT_STATE's resolved_offset_hz, which is only ever the next
// beacon/CQ offset. `reason` is why `aborted` is true (e.g. "low battery",
// from wherever the abort was actually triggered) -- empty when not aborted.
// Sourced from the real abort site rather than assumed client-side, so a
// future second abort reason shows up as itself instead of every abort
// reading the same hardcoded phrase.
void porta_emit_tx_hud(bool active, bool aborted, bool writes_blocked,
                       const char* text, float power_w, float swr, int battery_pct,
                       int offset_hz, const char* reason);

// The host's own USB-C host port attached or detached a device (radio
// normally; briefly the sidekick during its own flash workflow). Pushed the
// instant usb_c_presence_host.cpp's event fires -- see porta_proto.h's
// PORTA_EVT_USB_STATUS for why this is a typed field, not a log line the
// browser pattern-matches. `kind` is UsbCKind's own wire value.
void porta_emit_usb_status(bool present, uint8_t kind, uint16_t vid, uint16_t pid);

// One SWR sweep progress event (docs/architecture/radio.md, The SWR sweep): `kind` and `reason` are porta_sweep_kind_t and
// porta_sweep_reason_t wire values. `swr_hundredths` is -1 where there is no reading.
void porta_emit_sweep(uint8_t kind, uint8_t range_index, uint32_t freq_hz, int swr_hundredths,
                      uint8_t reason);

// Link health, decoding bytes *from* the sidekick: a CRC failure here means a
// corrupt frame arrived and was silently dropped (porta_proto.h's decoder
// never rescans a bad frame's bytes). Counts, not events. handle_config_get()
// in porta.cpp appends these to every board's CONFIG_GET reply.
uint32_t porta_crc_errors();
uint32_t porta_frames_ok();

// ---------------------------------------------------------------------------
// FILE_LIST / FILE_READ row emitters (I28d, docs/architecture/webapp.md (Screens)). The request/reply
// itself is asynchronous -- a directory listing runs on a background task
// and a file read is paced across many main-loop ticks, so neither can
// answer in the same call that received the request the way every other
// ACTION does. main.cpp's async FS pump calls these as rows become ready,
// then porta_emit_ack()/porta_emit_nak() (verb = PORTA_MSG_FILE_LIST or
// PORTA_MSG_FILE_READ) once done -- the same "burst then ACK" shape
// CONFIG_GET-all already uses, just spread across ticks instead of one call.
// ---------------------------------------------------------------------------

void porta_emit_file_name(const char* name);
void porta_emit_file_entry(const porta_qso_entry_row_t& row);
void porta_emit_ack(uint8_t verb);
void porta_emit_nak(uint8_t verb, const char* reason);
