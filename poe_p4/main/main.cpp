#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "amp_band_prime.h"
#include "autoseq.h"
#include "callsign_hashtable.h"
#include "decode_pipeline.h"
#include "debug_log.h"
#include "decode_tx_state.h"
#include "ft8_audio_pipeline.h"
#include "ft8_block_activity.h"
#include "porta.h"
#include "porta_host.h"
#include "protocol.h"
#include "qmx_transport.h"
#include "rtc_core.h"
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

static_assert(FT8_ACTIVITY_BATCH_MAX <= PORTA_ACTIVITY_LEVELS_MAX,
              "an activity batch must fit one ACTIVITY event");
static_assert(FT8_ACTIVITY_CELLS == PORTA_ACTIVITY_CELLS && FT8_ACTIVITY_MASK_BYTES == PORTA_ACTIVITY_MASK_BYTES,
              "the pipeline's frequency mask is the event's");

static void send_activity(void * /*ctx*/, int64_t slot_idx, int block_ms, int first,
                          const uint8_t *levels, const uint8_t *masks, int n, int lo_hz, int hi_hz) {
    porta_emit_activity((uint32_t)slot_idx, (uint8_t)block_ms, (uint8_t)first, levels, masks,
                        (uint8_t)n, (uint16_t)lo_hz, (uint16_t)hi_hz);
}

static void decode_audio_task(void * /*arg*/) {
    ft8_audio_pipeline_config_t pipe_cfg = {
        .tag = TAG,
        .ctx = nullptr,
        .read = qmx_audio_read_ft8_samples,
        .should_stop = qmx_audio_should_stop,
        .on_block_processed = nullptr,
        .on_activity = send_activity,
        .take_frontend_us = qmx_audio_take_frontend_us,
        .rx_path_for_slot = qmx_audio_rx_path_for_slot,
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
    // The capture path starts on what the settings give for this slot; after that the main loop moves it
    // at slot boundaries (qmx_audio_rx_tick, below; architecture/decode.md, I/Q front end).
    qmx_audio_rx_init(porta_p4_station_qmx_iq(), porta_p4_station_qmx_iq_ab());
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
        amp_band_prime_tick(esp_timer_get_time() / 1000);
        // A sweep request accepted just before a TX started waits for it to end; one already
        // running is always ticked, so it can de-key.
        if (!decode_tx_state_get_tx_active() || swr_sweep_active()) {
            swr_sweep_tick(esp_timer_get_time() / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        // The receive path for the slot about to begin, from qmx_iq and qmx_iq_ab as they read now: the
        // radio is told 0.5 s before its boundary, the capture follows at the boundary. Q9 at each CAT
        // link-up too: the radio's receive output follows the path, both ways, and is read back.
        const bool rx_busy = decode_tx_state_get_tx_active() || porta_p4_tune_active() ||
                             amp_band_prime_busy() || swr_sweep_busy();
        qmx_audio_rx_tick(porta_p4_station_qmx_iq(), porta_p4_station_qmx_iq_ab(), rx_busy);
        switch (radio_control_rx_path_tick(qmx_audio_rx_want_iq(), rx_busy)) {
            case RADIO_RX_PATH_MISMATCH:
                debug_log_line("Q9 read-back differs from the wanted receive path: the radio is in the other receive mode, "
                               "so nothing will decode");
                break;
            case RADIO_RX_PATH_NO_REPLY:
                debug_log_line("Q9; unanswered: cannot confirm the QMX receive mode matches the wanted path");
                break;
            case RADIO_RX_PATH_SENT: {
                // How long before the next boundary the radio was told: about 500 ms when a change
                // was scheduled, anything at a link-up.
                const int64_t slot_ms = g_protocol->slot_time_ms;
                const int64_t now = rtc_now_ms();
                char line[40];
                snprintf(line, sizeof(line), "Q9 %d left%lld", qmx_audio_rx_want_iq() ? 1 : 0,
                         (long long)((now / slot_ms + 1) * slot_ms - now));
                debug_log_line(line);
                break;
            }
            case RADIO_RX_PATH_NONE:
            case RADIO_RX_PATH_MATCHED:
                break;
        }
        // Set by qmx_transport on every CAT open; retried until it lands.
        if (decode_tx_state_get_cdc_initial_sync_pending() && radio_control_ready() &&
            !decode_tx_state_get_tx_active() && !porta_p4_tune_active() && !amp_band_prime_busy() &&
            !swr_sweep_busy() &&
            porta_p4_sync_vfo("QMX connect") == nullptr) {
            decode_tx_state_set_cdc_initial_sync_pending(false);
        }
        if (++ticks >= 500) {  // ~5 s at 10 ms
            ticks = 0;
            ESP_LOGI(TAG, "alive call=%s", g_call.c_str());
        }
    }
}
