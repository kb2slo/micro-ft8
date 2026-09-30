#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The seam between radio_control_{qmx,qdx}.cpp and whatever main board they
// run on: a QMX/QDX is a plain USB CDC-ACM serial device, but how that CDC
// connection gets opened is board-specific (the ADV's stream_uac.cpp shares
// the port with a UAC audio pipeline and its own USB host lifecycle; a
// headless board has neither). Extracted from main/audio/stream_uac.h (RFC
// 0005 Phase 2, docs/STYLE.md (Two boards, one source)) so radio_control itself
// depends on this narrow interface, not on one board's whole audio stack.
//
// Replies are parsed by cat_reply_parser.h, which each board's transport feeds from its CDC callback.
//
// Two groups, because the two backends need different halves: QMX tells the
// radio a tone frequency over CAT and the radio generates it, so
// radio_control_qmx.cpp only needs the cat_cdc_* group. QDX instead needs an
// actual audio waveform from the host, so radio_control_qdx.cpp also needs
// the uac_tx_* group. A board that only implements the QMX backend (like
// poe_p4 today) still must provide all eight symbols for the link to
// succeed, since this component compiles radio_control_qdx.cpp
// unconditionally -- an honest "not supported" implementation is fine; a
// missing symbol is not.

bool cat_cdc_ready(void);
esp_err_t cat_cdc_send(const uint8_t* data, size_t len, uint32_t timeout_ms);
void cat_cdc_reset_parsed_meters(void);
bool cat_cdc_get_parsed_meters(int* pc_tenths, int* sw_hundredths);
// The last FA reply (the VFO A frequency in Hz); false when none has arrived since the last reset.
bool cat_cdc_get_parsed_freq(int* hz);

bool uac_tx_begin_cpfsk(float base_hz,
                        const uint8_t* symbols,
                        size_t symbol_count,
                        float tone_spacing_hz,
                        uint32_t samples_per_symbol);
bool uac_tx_begin_tune(float tone_hz);
void uac_tx_end(void);

#ifdef __cplusplus
}
#endif
