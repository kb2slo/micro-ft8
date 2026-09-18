#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "autoseq.h"
#include "callsign_hashtable.h"
#include "ft8_audio_pipeline.h"
#include "protocol.h"
#include "qmx_transport.h"
#include "radio_control.h"

// RFC 0005 Phase 2: uses the shared components/radio_control instead of
// duplicating CAT logic in this file (Phase 1's ad hoc sequence -- see git
// history -- is exactly the "copy the function into a second file" pattern
// RFC 0005 §5 exists to rule out). Board-specific USB/CDC/UAC bring-up lives
// in qmx_transport.cpp; everything CAT-protocol-shaped and the decode/
// autoseq pipeline itself (components/decode_pipeline,
// components/ft8_audio_pipeline) are shared with the ADV. TX (the tone-
// scheduling state machine, still main.cpp-only on the ADV) is the next
// milestone, not this one -- see RFC 0005's Phase 2 status amendment,
// 2026-09-17.

static const char *TAG = "poe_p4";

static constexpr int kQmxFreqHz = 7074000;  // 40m FT8, same test frequency RFC 0001 §4.6 used

// g_protocol (protocol.h): defined once per board, same as the ADV's
// main.cpp. FT8 only -- this board doesn't have FT4 mode-switch UI yet.
const ProtocolConfig* g_protocol = &kProtocolFT8;

// The globals decode_pipeline/ft8_audio_pipeline bare-forward-declare and
// expect some board to define (see decode_pipeline.cpp's comment on why).
// g_call is a placeholder until a real config path exists (porta/sidekick,
// RFC 0005 Phase 3) -- RX decode and mycall-matching work either way; only
// auto-reply (not wired this milestone -- see arm_from_autoseq_or_beacon's
// stub in decode_seams.cpp) would need it to be real.
std::string g_call = "N0CALL";
volatile bool g_tx_view_dirty = false;
std::string g_last_reply_text;
bool g_rx_dirty = false;

static void decode_audio_task(void * /*arg*/) {
    ft8_audio_pipeline_config_t pipe_cfg = {
        .tag = TAG,
        .ctx = nullptr,
        .read = qmx_audio_read_ft8_samples,
        .should_stop = qmx_audio_should_stop,
        .on_block_processed = nullptr,
    };
    // Waits out here, not inside the pipeline: ft8_audio_pipeline_run()
    // expects streaming audio for its slot-timing math from the moment it's
    // called, and QMX's UAC interface takes a few seconds to enumerate after
    // qmx_transport_start().
    while (!qmx_audio_ready()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "QMX UAC streaming; starting decode pipeline");
    ft8_audio_pipeline_run(&pipe_cfg);
    ESP_LOGW(TAG, "decode_audio_task: pipeline exited (QMX audio disconnected)");
    vTaskDelete(NULL);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Micro-FT8 PoE-P4 (Phase 2: shared decode/TX core, RX half) booted");

    hashtable_init();
    autoseq_init();

    radio_control_set_backend(RADIO_CONTROL_QMX);
    qmx_transport_start();

    xTaskCreatePinnedToCore(decode_audio_task, "decode_audio", 8192, nullptr, 4, nullptr, 1);

    bool synced = false;
    int ticks = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!synced && radio_control_ready()) {
            esp_err_t err = radio_control_sync_frequency_mode(kQmxFreqHz);
            ESP_LOGI(TAG, "radio_control_sync_frequency_mode(%d) -> %s", kQmxFreqHz, esp_err_to_name(err));
            if (err == ESP_OK) {
                synced = true;
                ESP_LOGI(TAG, "check the QMX's own screen for %d Hz", kQmxFreqHz);
            }
        }
        if (++ticks >= 5) {
            ticks = 0;
            ESP_LOGI(TAG, "alive");
        }
    }
}
