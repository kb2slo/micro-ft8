#pragma once

// Turns the bytes a QMX/QDX sends back over CAT into the few replies this
// firmware reads: PC (power, tenths of a watt), SW (SWR, hundredths), FA
// (the VFO A frequency) and SR (the SWR-protection lock: 1 locked, 0 clear). Everything else, including the radio's "?;" for a
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
    char frame[CAT_REPLY_FRAME_MAX];
    size_t frame_len;
    int escape_state;  // 0 text, 1 after ESC, 2 inside a CSI sequence
} cat_reply_parser_t;

// Forgets every reply and any half-received frame.
void cat_reply_parser_reset(cat_reply_parser_t* parser);

void cat_reply_parser_feed(cat_reply_parser_t* parser, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
