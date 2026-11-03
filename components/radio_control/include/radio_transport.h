#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The seam between radio_control_qmx.cpp and whatever main board it
// runs on: a QMX is a plain USB CDC-ACM serial device, but how that CDC
// connection gets opened is board-specific (a board that also streams UAC audio shares the port with
// that pipeline and its USB host lifecycle). Extracted from main/audio/stream_uac.h (RFC
// 0005 Phase 2, docs/STYLE.md (Two boards, one source)) so radio_control itself
// depends on this narrow interface, not on one board's whole audio stack.
//
// Replies are parsed by cat_reply_parser.h, which each board's transport feeds from its CDC callback.
//
// The QMX is told each tone's frequency over CAT and generates it itself, so the CAT group below is the
// whole seam.

bool cat_cdc_ready(void);
esp_err_t cat_cdc_send(const uint8_t* data, size_t len, uint32_t timeout_ms);
void cat_cdc_reset_parsed_meters(void);
bool cat_cdc_get_parsed_meters(int* pc_tenths, int* sw_hundredths);
// The last FA reply (the VFO A frequency in Hz); false when none has arrived since the last reset.
bool cat_cdc_get_parsed_freq(int* hz);
// The last SR reply (1 the SWR-protection lock is set, 0 clear); false when none has arrived since the last reset.
bool cat_cdc_get_parsed_sr(int* lock);
// Forgets the last IF reply only; the meters and the SWR lock keep theirs.
void cat_cdc_reset_parsed_if(void);
// The radio's firmware version from its VN reply ("1_04_004"), copied into `out`; false when it
// has not answered since the radio connected.
bool cat_cdc_get_version(char* out, size_t cap);
// How many IF replies have differed from the one before, since boot. A reader that remembers the
// count it last saw learns of a change without a clear that could lose one.
uint32_t cat_cdc_if_changes(void);
// The last IF reply; false when none has arrived since the last reset. tx 1 = transmitting, mode is the MD digit, split 1 = on.
bool cat_cdc_get_parsed_if(int* hz, int* tx, int* mode, int* split);

#ifdef __cplusplus
}
#endif
