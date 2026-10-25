#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "autoseq.h"
#include "callsign_hashtable.h"
#include "decode_pipeline.h"
#include "debug_log.h"
#include "decode_tx_state.h"
#include "ft8_audio_pipeline.h"
#include "porta.h"
#include "porta_host.h"
#include "protocol.h"
#include "qmx_transport.h"
#include "radio_control.h"
#include "storage_service.h"
#include "swr_sweep.h"
#include "tx_scheduler.h"

// docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)): shared radio_control + decode; porta (B67) on Grove
// G53/G54. TX tone scheduling (B68) is still next.

static const char *TAG = "poe_p4";

const ProtocolConfig* g_protocol = &kProtocolFT8;

// Default until Station.txt loads; porta_host overwrites on CONFIG_SET.
std::string g_call = "YOURCALL";

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
        porta_p4_set_decode_failed(false);
        if (!ft8_audio_pipeline_run(&pipe_cfg)) {
            // Could not start (RAM, a task), so nothing will decode until it can. Say so once, on
            // the phone as well as here, and wait before trying again rather than every 60 ms.
            ESP_LOGE(TAG, "decode pipeline could not start: the radio is up but nothing will decode");
            debug_log_line("DECODE pipeline could not start: no RAM or task; retrying every 10 s");
            porta_p4_set_decode_failed(true);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
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

    int ticks = 0;
    while (true) {
        porta_tick();
        porta_p4_companion_status_tick();
        porta_p4_tx_hud_tick();
        porta_p4_radio_push_tick();
        porta_p4_file_tick();
        // The decode task hands each slot here: this loop alone touches autoseq
        // and the armed TX (docs/architecture/main_board.md, P4).
        decode_pipeline_apply();
        tx_scheduler_tick();
        // A sweep request accepted just before a TX started waits for it to end; one already
        // running is always ticked, so it can de-key.
        if (!decode_tx_state_get_tx_active() || swr_sweep_active()) {
            swr_sweep_tick(esp_timer_get_time() / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        // Set by qmx_transport on every CAT open; retried until it lands.
        if (decode_tx_state_get_cdc_initial_sync_pending() && radio_control_ready() &&
            !decode_tx_state_get_tx_active() && !swr_sweep_busy() &&
            porta_p4_sync_vfo("QMX connect") == nullptr) {
            decode_tx_state_set_cdc_initial_sync_pending(false);
        }
        if (++ticks >= 500) {  // ~5 s at 10 ms
            ticks = 0;
            ESP_LOGI(TAG, "alive call=%s", g_call.c_str());
        }
    }
}
