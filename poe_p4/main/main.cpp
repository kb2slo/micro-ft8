#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// RFC 0005 Phase 0 skeleton: proves `idf.py build` targets esp32p4 in a
// project of its own, mirroring sidekick's separate-project pattern. No
// radio, no decode, no UI -- see docs/rfcs/0005-p4-poe-headless-port.md.

static const char *TAG = "poe_p4";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Micro-FT8 PoE-P4 (Phase 0 skeleton) booted");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
