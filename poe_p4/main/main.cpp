#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "autoseq.h"
#include "callsign_hashtable.h"
#include "ft8_audio_pipeline.h"
#include "porta.h"
#include "porta_p4_host.h"
#include "protocol.h"
#include "qmx_transport.h"
#include "radio_control.h"
#include "storage_service.h"
#include "tx_scheduler.h"

// RFC 0005 Phase 2/3: shared radio_control + decode; porta (B67) on Grove
// G53/G54. TX tone scheduling (B68) is still next.

static const char *TAG = "poe_p4";

const ProtocolConfig* g_protocol = &kProtocolFT8;

// Seeded to ADV's Station.txt default; porta_host overwrites on CONFIG_SET.
std::string g_call = "YOURCALL";
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
    // QMX CAT/UAC comes back only after a radio power cycle (QMX quirk);
    // keep this task alive so a replug restarts decode without resetting P4.
    while (true) {
        while (!qmx_audio_ready()) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ESP_LOGI(TAG, "QMX UAC streaming; starting decode pipeline");
        ft8_audio_pipeline_run(&pipe_cfg);
        ESP_LOGW(TAG, "decode_audio_task: pipeline exited (QMX audio disconnected); waiting for UAC");
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Micro-FT8 PoE-P4 (Phase 3: porta/companion) booted");

    const esp_err_t storage_err = storage_service_init();
    if (storage_err != ESP_OK) {
        ESP_LOGE(TAG, "storage_service_init failed: %s", esp_err_to_name(storage_err));
    }

    hashtable_init();
    autoseq_init();
    porta_p4_station_boot();
    porta_p4_adif_register();

    radio_control_set_backend(RADIO_CONTROL_QMX);
    qmx_transport_start();
    porta_start();

    xTaskCreatePinnedToCore(decode_audio_task, "decode_audio", 8192, nullptr, 4, nullptr, 1);

    bool synced = false;
    int ticks = 0;
    while (true) {
        porta_tick();
        porta_p4_companion_status_tick();
        porta_p4_tx_hud_tick();
        porta_p4_file_tick();
        tx_scheduler_tick();
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!synced && radio_control_ready()) {
            const int freq_hz = porta_p4_station_freq_hz();
            esp_err_t err = radio_control_sync_frequency_mode(freq_hz);
            ESP_LOGI(TAG, "radio_control_sync_frequency_mode(%d) -> %s", freq_hz,
                     esp_err_to_name(err));
            if (err == ESP_OK) {
                synced = true;
                ESP_LOGI(TAG, "check the QMX's own screen for %d Hz", freq_hz);
            }
        }
        if (++ticks >= 500) {  // ~5 s at 10 ms
            ticks = 0;
            ESP_LOGI(TAG, "alive call=%s", g_call.c_str());
        }
    }
}
