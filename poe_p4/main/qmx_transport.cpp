#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmx_transport.h"
#include "radio_transport.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

// RFC 0005 Phase 2: this board's implementation of the radio_transport.h
// seam components/radio_control needs (RFC 0005 §5's shared-code goal) --
// USB host and CDC-ACM lifecycle are board-specific (the ADV shares a port
// with a UAC audio pipeline and its own presence-detection client; this
// board has neither yet), so that part stays here while everything
// CAT-protocol-shaped lives in the shared component instead of being
// duplicated. Only the QMX backend is wired up (main.cpp selects
// RADIO_CONTROL_QMX); the QDX-only uac_tx_* functions below are honestly
// unimplemented, not silently faked -- see the comment on them.

static const char *TAG = "qmx_transport";

static constexpr uint16_t kQmxVid = 0x0483;
static constexpr uint16_t kQmxPid = 0xA34C;

static cdc_acm_dev_hdl_t s_cdc_handle = nullptr;
static int64_t s_last_open_attempt_ms = 0;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void * /*user_ctx*/) {
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "QMX CDC disconnected");
        // Field-found 2026-09-17: cdc_acm_host_close() must be called even
        // though the device is already gone, or the driver's interface/
        // endpoint claim for this handle leaks. The next cdc_acm_host_open()
        // then fails to claim the interface (EP alloc / claim errors), and
        // repeated retries against that leaked state eventually hang
        // usb_lib_task inside descriptor parsing (watchdog reboot loop).
        // Matches the ADV's own cdc_close() in main/audio/stream_uac.cpp,
        // which already does this unconditionally.
        cdc_acm_host_close(s_cdc_handle);
        s_cdc_handle = nullptr;
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGW(TAG, "QMX CDC error: %d", event->data.error);
    }
}

static bool cdc_data_cb(const uint8_t *data, size_t data_len, void * /*user_arg*/) {
    ESP_LOGI(TAG, "QMX CAT rx (%u bytes): %.*s", (unsigned)data_len, (int)data_len, data);
    return true;
}

static void try_open(void) {
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
        try_open();
    }
}

void qmx_transport_start(void) {
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 5, nullptr, 0);
}

// radio_transport.h -- CAT group, real.

bool cat_cdc_ready(void) {
    return s_cdc_handle != nullptr;
}

esp_err_t cat_cdc_send(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!s_cdc_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return cdc_acm_host_data_tx_blocking(s_cdc_handle, data, len, timeout_ms);
}

void cat_cdc_reset_parsed_meters(void) {
    // QMX power/SWR meter parsing (PC;SW; replies) isn't built on this board
    // yet -- Phase 2 only needs frequency-set to work. Safe no-op, not a
    // silent lie: get_parsed_meters() below always reports "no reading".
}

bool cat_cdc_get_parsed_meters(int * /*pc_tenths*/, int * /*sw_hundredths*/) {
    return false;
}

// radio_transport.h -- UAC TX group, QDX-only. components/radio_control
// compiles radio_control_qdx.cpp unconditionally, so these must exist for
// the link to succeed, but main.cpp never selects RADIO_CONTROL_QDX on this
// board, so they are never actually called.

bool uac_tx_begin_cpfsk(float /*base_hz*/, const uint8_t * /*symbols*/, size_t /*symbol_count*/,
                        float /*tone_spacing_hz*/, uint32_t /*samples_per_symbol*/) {
    return false;
}

bool uac_tx_begin_tune(float /*tone_hz*/) {
    return false;
}

void uac_tx_end(void) {}
