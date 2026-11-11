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
int qmx_audio_read_ft8_samples(void *ctx, float *const *out, int n_channels, int max_samples);
// The FT4 channel's dial against FT8's, for the second channel the read fills (docs/architecture/decode.md,
// FT4). Set before a pipeline with two channels starts; the read sets that channel's front end up from it.
void qmx_audio_set_ft4_dial_offset_hz(int32_t dial_offset_hz);
bool qmx_audio_should_stop(void *ctx);

// ft8_audio_pipeline_config_t::take_frontend_us: the front end's time since the last call.
uint32_t qmx_audio_take_frontend_us(void *ctx);

#ifdef __cplusplus
}
#endif
