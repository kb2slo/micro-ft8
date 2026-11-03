#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts usb_lib_task (USB Host + CDC-ACM event pump) and uac_lib_task (UAC
// open/settle/start, queue-woken), then polls for the QMX (VID:0x0483
// PID:0xA34C) on the Host port -- one composite device, two class drivers on
// the same usb_host_install() (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)): "the QMX ISO split" FIFO tuning so UAC can attach without
// reinstalling the host). The events pump must never
// block on UAC start. Once open, radio_control's cat_cdc_ready()/cat_cdc_send()
// (radio_transport.h) and this file's qmx_audio_* below start working.
void qmx_transport_start(void);

// True once the QMX's UAC RX (microphone) interface is open and streaming.
bool qmx_audio_ready(void);

// ft8_audio_pipeline_config_t's read/should_stop callbacks -- pass these
// directly. `ctx` is unused (nullptr is fine).
int qmx_audio_read_ft8_samples(void *ctx, float *out, int max_samples);
bool qmx_audio_should_stop(void *ctx);

// Which receive path the capture decodes, slot by slot (components/radio_control/include/qmx_rx_mode.h;
// architecture/decode.md, I/Q front end): the QMX's audio (the L/R average, decimated) or its I/Q (the
// receive front end, components/iq_frontend). Station `qmx_iq` and `qmx_iq_ab`.
//
// Once at boot, before qmx_transport_start(): the plan starts on the path the settings give for
// the current slot.
void qmx_audio_rx_init(bool qmx_iq, int ab_slots);
// The main loop, every pass: decides the path for the slot that begins within the next 0.5 s, from the
// settings as they read now. True when it changed; the radio is then told (qmx_audio_rx_want_iq()), and
// the capture follows at the boundary. `busy`: a change is not made now (a transmission, Tune, a sweep
// or a prime is running).
bool qmx_audio_rx_tick(bool qmx_iq, int ab_slots, bool busy);
// The path the radio should be in: the latest decision (the next slot's, in the last 0.5 s of a slot).
bool qmx_audio_rx_want_iq(void);
// ft8_audio_pipeline_config_t::rx_path_for_slot: 1 audio, 2 I/Q.
int qmx_audio_rx_path_for_slot(void *ctx, int64_t slot_idx);

// ft8_audio_pipeline_config_t::take_frontend_us: the front end's time since the last call.
uint32_t qmx_audio_take_frontend_us(void *ctx);

#ifdef __cplusplus
}
#endif
