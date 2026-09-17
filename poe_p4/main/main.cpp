#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmx_transport.h"
#include "radio_control.h"

// RFC 0005 Phase 2: uses the shared components/radio_control instead of
// duplicating CAT logic in this file (Phase 1's ad hoc sequence -- see git
// history -- is exactly the "copy the function into a second file" pattern
// RFC 0005 §5 exists to rule out). Board-specific USB/CDC bring-up lives in
// qmx_transport.cpp; everything CAT-protocol-shaped is shared with the ADV.

static const char *TAG = "poe_p4";

static constexpr int kQmxFreqHz = 7074000;  // 40m FT8, same test frequency RFC 0001 §4.6 used

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Micro-FT8 PoE-P4 (Phase 2: shared radio_control) booted");
    radio_control_set_backend(RADIO_CONTROL_QMX);
    qmx_transport_start();

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
