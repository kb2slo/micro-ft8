#pragma once

// Turns the bytes a QMX sends back over CAT into the few replies this
// firmware reads: PC (power, tenths of a watt), SW (SWR, hundredths), FA
// (the VFO A frequency), SR (the SWR-protection lock: 1 locked, 0 clear), Q9 (the QMX's receive output: 1 IQ, 0 audio), IF (the radio's
// state in one reply: frequency, TX or RX, mode, split), VN (its firmware version) and MM (a Menu Manager value). The radio's "?;"
// for a command it refused is only counted. Everything else is ignored.
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
    volatile int q9_iq;
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
    // MM: the last Menu Manager reply's value ("ENABLED", "40", "QMX+ 160-6m"), and how many MM
    // replies have arrived since boot. The value is written before the count, so a reader that
    // sees the count move reads the value that came with it. Never reset: a reader remembers the
    // count it saw when it asked.
    char mm[CAT_REPLY_FRAME_MAX];
    volatile uint32_t mm_replies;
    // How many "?;" replies (a command refused) have arrived since boot. Never reset, as mm_replies.
    volatile uint32_t refusals;
    char frame[CAT_REPLY_FRAME_MAX];
    size_t frame_len;
    int escape_state;  // 0 text, 1 after ESC, 2 inside a CSI sequence
} cat_reply_parser_t;

// Every field at its starting value: no reading, no version, no frame. Use this to construct a
// parser, not a positional initializer, which shifts when a field is added.
void cat_reply_parser_init(cat_reply_parser_t* parser);

// Forgets every reading and any half-received frame. Not the firmware version, and not Q9: like
// the version, Q9 is set by a command at link-up and a transmission's end must not lose its answer.
void cat_reply_parser_reset(cat_reply_parser_t* parser);

// Forgets the firmware version.
void cat_reply_parser_forget_version(cat_reply_parser_t* parser);

void cat_reply_parser_feed(cat_reply_parser_t* parser, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
