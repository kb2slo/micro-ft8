#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

// RFC 0005 Phase 0/B59: minimal generic USB host bring-up to bench-confirm
// the U213's two-port claim (docs/rfcs/0005-p4-poe-headless-port.md §4/§7) --
// that the dedicated Host port can enumerate a device while the console
// (this log, over the Download/OTG port) keeps running. Deliberately generic
// (any USB device, not a QMX) and deliberately not QMX/CAT-specific: that is
// Phase 1, gated on this row, and belongs in components/ once it lands
// (RFC 0005 §5), not here. No custom FIFO tuning -- that is an S3+QMX-tuned
// choice in the ADV's own usb_lib_task (main/audio/stream_uac.cpp) that has
// no basis to carry over blind to different silicon and a generic device.

static const char *TAG = "poe_p4";

static usb_host_client_handle_t s_client = nullptr;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void * /*arg*/) {
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        usb_device_handle_t dev = nullptr;
        if (usb_host_device_open(s_client, event_msg->new_dev.address, &dev) != ESP_OK || !dev) {
            ESP_LOGW(TAG, "open addr %u failed", event_msg->new_dev.address);
            return;
        }
        const usb_device_desc_t *desc = nullptr;
        if (usb_host_get_device_descriptor(dev, &desc) == ESP_OK && desc) {
            ESP_LOGI(TAG, "USB attach VID:0x%04x PID:0x%04x", desc->idVendor, desc->idProduct);
        } else {
            ESP_LOGI(TAG, "USB attach (descriptor read failed)");
        }
        usb_host_device_close(s_client, dev);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "USB detach");
        break;
    default:
        break;
    }
}

static void usb_client_task(void * /*arg*/) {
    while (true) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(100));
    }
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

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = nullptr,
        },
    };
    err = usb_host_client_register(&client_config, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    xTaskCreatePinnedToCore(usb_client_task, "usb_client", 3072, nullptr, 4, nullptr, 0);

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Micro-FT8 PoE-P4 (Phase 0 skeleton) booted");
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 5, nullptr, 0);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
