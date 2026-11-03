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

// Chooses what the capture path decodes, once at boot before qmx_transport_start(): false the
// QMX's audio (the L/R average, decimated), true its I/Q (the receive front end,
// components/iq_frontend). Station `qmx_iq`; a change takes a reboot. architecture/decode.md,
// I/Q front end.
void qmx_audio_set_iq(bool iq);
bool qmx_audio_iq_enabled(void);

// ft8_audio_pipeline_config_t::take_frontend_us: the front end's time since the last call.
uint32_t qmx_audio_take_frontend_us(void *ctx);

#ifdef __cplusplus
}
#endif
