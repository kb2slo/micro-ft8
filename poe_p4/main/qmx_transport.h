#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
