#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ft8_audio_pipeline.h"
#include "radio_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UAC_SAMPLE_RATE 48000
#define UAC_BIT_RESOLUTION 24
#define UAC_CHANNELS 2

#define UAC_BUFFER_SIZE 4000
#define UAC_BUFFER_THRESHOLD 600

typedef enum {
    UAC_STATE_IDLE,
    UAC_STATE_WAITING,
    UAC_STATE_CONNECTED,
    UAC_STATE_STREAMING,
    UAC_STATE_ERROR,
} uac_stream_state_t;

typedef enum {
    UAC_PROFILE_QMX = 0,
    UAC_PROFILE_GENERIC_USB = 1,
} uac_stream_profile_t;

#define UAC_WATERFALL_ROW_WIDTH FT8_AUDIO_WATERFALL_ROW_WIDTH

bool uac_is_streaming(void);
// rtc_now_ms() at the last successful USB audio read (bytes_read > 0, a real
// frame off the wire, not a quiet band -- silence still reads real bytes).
// 0 if no frame has ever arrived this session.
int64_t uac_last_rx_ms(void);
bool uac_start_with_profile(uac_stream_profile_t profile);
void uac_stop(void);
esp_err_t uac_host_ensure_started(void);
esp_err_t uac_ensure_host_uninstalled(void);

// cat_cdc_*/uac_tx_* declared in radio_transport.h; this file provides the
// ADV's implementation (stream_uac.cpp) but does not redeclare the interface.

#ifdef __cplusplus
}
#endif
