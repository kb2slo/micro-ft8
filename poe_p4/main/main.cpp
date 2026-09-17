#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

// RFC 0005 Phase 1: USB host QMX bring-up -- the actual B26-successor bench
// test (docs/rfcs/0005-p4-poe-headless-port.md). Supersedes B59's generic
// USB-host scaffolding (main.cpp git history) now that the two-port claim
// it existed to prove is confirmed. Sends the same safe, receive-side-only
// CAT sequence RFC 0001 §4.6's AtomS3 test and this project's own
// main/radio/radio_control_qmx.cpp (qmx_sync_frequency_mode()) use:
// MD6; FR0; FT0; FA<freq>; -- no TX;, safe with no antenna. Pass condition
// is the QMX's own screen showing the new frequency, not this log.

static const char *TAG = "poe_p4";

static constexpr uint16_t kQmxVid = 0x0483;
static constexpr uint16_t kQmxPid = 0xA34C;
static constexpr int kQmxFreqHz = 7074000;  // 40m FT8, same test frequency RFC 0001 §4.6 used

static cdc_acm_dev_hdl_t s_cdc_handle = nullptr;
static int64_t s_last_open_attempt_ms = 0;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void * /*user_ctx*/) {
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "QMX CDC disconnected");
        s_cdc_handle = nullptr;
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGW(TAG, "QMX CDC error: %d", event->data.error);
    }
}

static bool cdc_data_cb(const uint8_t *data, size_t data_len, void * /*user_arg*/) {
    ESP_LOGI(TAG, "QMX CAT rx (%u bytes): %.*s", (unsigned)data_len, (int)data_len, data);
    return true;
}

static void qmx_send(const char *cmd) {
    if (!s_cdc_handle) {
        return;
    }
    esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc_handle, (const uint8_t *)cmd, strlen(cmd), 200);
    ESP_LOGI(TAG, "QMX CAT tx '%s' -> %s", cmd, esp_err_to_name(err));
}

static void qmx_try_open_and_test(void) {
    if (s_cdc_handle) {
        return;
    }
    int64_t t = now_ms();
    if (s_last_open_attempt_ms != 0 && (t - s_last_open_attempt_ms) < 1000) {
        return;
    }
    s_last_open_attempt_ms = t;

    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 64,
        .in_buffer_size = 64,
        .event_cb = cdc_event_cb,
        .data_cb = cdc_data_cb,
        .user_arg = nullptr,
    };
    cdc_acm_dev_hdl_t handle = nullptr;
    esp_err_t err = cdc_acm_host_open(kQmxVid, kQmxPid, 0, &dev_cfg, &handle);
    if (err != ESP_OK) {
        return;  // not attached yet -- keep polling
    }
    s_cdc_handle = handle;
    ESP_LOGI(TAG, "QMX CDC-ACM opened (VID:0x%04x PID:0x%04x iface 0)", kQmxVid, kQmxPid);

    qmx_send("MD6;");
    vTaskDelay(pdMS_TO_TICKS(50));
    qmx_send("FR0;");
    vTaskDelay(pdMS_TO_TICKS(50));
    qmx_send("FT0;");
    vTaskDelay(pdMS_TO_TICKS(50));
    char fa[32];
    snprintf(fa, sizeof(fa), "FA%011d;", kQmxFreqHz);
    qmx_send(fa);
    ESP_LOGI(TAG, "QMX CAT sequence sent -- check the QMX's own screen for %d Hz", kQmxFreqHz);
}

static void usb_lib_task(void * /*arg*/) {
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB Host installed");

    const cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,
        .new_dev_cb = nullptr,
    };
    err = cdc_acm_host_install(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM driver installed");

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        qmx_try_open_and_test();
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Micro-FT8 PoE-P4 (Phase 1: QMX CAT bring-up) booted");
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 5, nullptr, 0);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
