#pragma once

// Turns the bytes a QMX/QDX sends back over CAT into the few replies this
// firmware reads: PC (power, tenths of a watt), SW (SWR, hundredths), FA
// (the VFO A frequency), SR (the SWR-protection lock: 1 locked, 0 clear), IF (the radio's
// state in one reply: frequency, TX or RX, mode, split) and VN (its firmware version). Everything else, including the radio's "?;" for a
// command it refused, is ignored.
//
// The radio appends terminal display codes (ESC [ ... final byte, for example
// ESC[7m) after some replies. They are dropped here; left in, they would glue
// onto the front of the next reply and hide its prefix.
//
// No allocation and no locking: feed() runs in the USB callback and the
// getters run elsewhere, so each reading is a single int store (-1 = none).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAT_REPLY_FRAME_MAX 48

typedef struct {
    volatile int pc_tenths;
    volatile int sw_hundredths;
    volatile int fa_hz;
    volatile int sr_lock;
    // IF: -1 until a reply arrives. tx 1 = transmitting; mode is the MD digit; split 1 = on.
    volatile int if_hz;
    volatile int if_tx;
    volatile int if_mode;
    volatile int if_split;
    // Counts IF replies that differ from the one before (a push from AI2; or an answer to IF;).
    // Never reset, so a reader that remembers the count it saw cannot lose a change to a clear.
    volatile uint32_t if_changes;
    // VN: the radio's firmware version, such as "1_04_004"; empty until it answers. Unlike the
    // readings above, reset() leaves it: it is a fact about the radio, not about the last
    // transmission. forget_version() clears it when the radio goes.
    char vn[16];
    char frame[CAT_REPLY_FRAME_MAX];
    size_t frame_len;
    int escape_state;  // 0 text, 1 after ESC, 2 inside a CSI sequence
} cat_reply_parser_t;

// Forgets every reading and any half-received frame. Not the firmware version.
void cat_reply_parser_reset(cat_reply_parser_t* parser);

// Forgets the firmware version.
void cat_reply_parser_forget_version(cat_reply_parser_t* parser);

void cat_reply_parser_feed(cat_reply_parser_t* parser, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
