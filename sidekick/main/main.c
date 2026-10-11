#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "host_link.h"
#include "pairing_http.h"
#include "porta_proto.h"
#include "wifi_prov.h"

static const char *TAG = "sidekick";

// PORTA companion beacon — docs/architecture/sidekick.md (Port A: the link to the main board). One-way, sidekick -> main board: sync
// byte (never collides with NMEA's '$'-prefixed sentences, so the main board can
// tell GPS and companion apart by listening) + this build's version string
// (from esp_app_desc_t, the same git-SHA+dirty value tools/git_version.cmake
// computes for both sidekick and the main board, so an exact string match is
// meaningful) + a simple checksum so the main board isn't locking onto a single
// stray byte of line noise as "companion present".
//
// Grove pin roles mirror the main board's PORTA wiring: a Grove cable is
// straight-through (G1-to-G1, G2-to-G2), and the main board receives on its G1 /
// transmits on its G2, so sidekick must transmit on G1 / receive on G2 for
// the two ends to actually reach each other over the same two wires.
#define PORTA_UART UART_NUM_1
#define PORTA_TX_PIN GPIO_NUM_1  // G1
#define PORTA_RX_PIN GPIO_NUM_2  // G2
// 1 Hz made sense when the beacon was the entire conversation. It now shares
// the wire with the host's event stream, where it is noise -- and nothing needs
// a sub-5-second answer to "what version is the companion running".
#define PORTA_BEACON_INTERVAL_MS 5000
// The loop still ticks at 1 s so the alive line and anything else added later
// keep their cadence; only the beacon is slowed.
#define LOOP_TICK_MS 1000
#define BEACON_EVERY (PORTA_BEACON_INTERVAL_MS / LOOP_TICK_MS)
// One fill of the peer's TX ring is 4096 bytes. 2048 was about 10 ms at
// 2 Mbps: 100 KB passed, and 14 MB lost frames (2026-10-01, drops 227, crc 155).
#define PORTA_RX_BUF 8192

static void porta_beacon_init(void) {
    gpio_reset_pin(PORTA_TX_PIN);
    gpio_reset_pin(PORTA_RX_PIN);

    uart_config_t cfg = {
        .baud_rate = PORTA_BAUD,  // porta_proto.h, shared with the main board
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // RX was 256, smaller than one maximum frame (259). TX holds several of
    // those: the link test sends them back to back.
    ESP_ERROR_CHECK(uart_driver_install(PORTA_UART, PORTA_RX_BUF, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PORTA_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(PORTA_UART, PORTA_TX_PIN, PORTA_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// HELLO replaces the hand-rolled beacon. Same job -- tell the host which build
// is running here -- but framed and CRC-checked like every other message, and
// carrying a protocol version the old format had no room for. The two formats
// could not coexist: both begin with 0xC6, and the old one has no type or
// length, so the host would have read a framed message as a beacon.
static void porta_beacon_send(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    const size_t n = porta_proto_encode_hello(PORTA_PROTO_VERSION, desc->version,
                                              frame, sizeof(frame));
    if (n > 0) {
        uart_write_bytes(PORTA_UART, (const char *)frame, n);
    }
}

// PORTA update flash (docs/rfcs/rejected/0001-ble-companion.md §5.2c "Update flash over PORTA") — tried a
// button-hold self-restart (hold the on-board GPIO9 button, sidekick calls
// esp_restart() on its own), reasoning the operator's finger holding GPIO9
// low at that exact reset would land the chip in its ROM bootloader the
// same as a fresh cable plug-in. Bench-tested 2026-09-04: the button read
// and the restart both worked correctly, but the reboot banner showed
// `boot:0xd (SPI_FAST_FLASH_BOOT)` — a software (`SW_CPU`) reset does not
// cause the ROM to re-sample GPIO9 at all, so it boots straight back into
// the app regardless of the button. Corroborated by Espressif's own esptool
// disabling its (different, more aggressive) watchdog-reset trick
// specifically on ESP32-C6 for causing full system freezes — this looks
// like a genuine chip limitation, not a timing/implementation bug. Removed
// rather than left in as dead, misleading UI. Next real attempt: a custom
// second-stage bootloader that checks an RTC-memory flag and, if set, runs
// its own flash-receiver instead of the app — sidesteps ROM strap-sampling
// entirely, needs no wiring, but is real bootloader-level engineering
// (higher stakes: bugs there affect every boot, not just updates) — see
// docs/architecture/sidekick.md (Installing the sidekick from the main board)'s "Future phase" note. USB-C stays the only flash path
// until that's built.

// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE an image installed by OTA boots
// in PENDING_VERIFY and is rolled back on the next restart unless it declares
// itself good. "Good" here means the companion link works — an image that
// boots but cannot beacon is useless to the main board, so the beacon is the check
// rather than merely reaching app_main(). Images written over USB-C are not
// pending and this is a no-op for them.
static void mark_valid_once_beaconing(uint32_t beacons_sent)
{
    static bool done = false;
    if (done || beacons_sent < 3) {
        return;
    }
    done = true;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "Image marked valid (beacon confirmed); rollback cancelled");
    } else {
        ESP_LOGE(TAG, "Could not mark image valid — it will roll back on restart");
    }
}

// The AtomS3 Lite's user button -- the one that is not reset. GPIO41, read
// active low against the board's own pull-up: taken from the M5Unified this
// repo vendors, which configures it as plain input (not input_pullup) for
// board_M5AtomS3Lite and reads it inverted, rather than guessed from a
// pinout drawing.
#define BUTTON_PIN         GPIO_NUM_41
#define BUTTON_DEBOUNCE_US 250000

static volatile bool s_button_pressed;
static bool s_button_ready;

// Holding the button this many loop ticks resets the AP's name and password
// (row rgg2). Whole ticks of LOOP_TICK_MS, so the hold is one tick short of the
// seconds; a press for the pairing token is far shorter.
#define BUTTON_HOLD_TICKS WIFI_PROV_AP_RESET_HOLD_SECS
_Static_assert(LOOP_TICK_MS == 1000, "the hold counts one-second ticks");

// The edge is caught in an ISR and consumed by the loop below, because that
// loop ticks once a second and would miss most presses. The work itself is
// deferred rather than done here: pairing_http_open_disclosure() arms an
// esp_timer, and esp_timer_start_once() is not safe to call from an ISR.
static void button_isr(void *arg)
{
    (void)arg;
    static int64_t last_us;
    const int64_t  now_us = esp_timer_get_time();
    if (now_us - last_us < BUTTON_DEBOUNCE_US) {
        return;
    }
    last_us          = now_us;
    s_button_pressed = true;
}

static void button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Button config failed — pairing token cannot be read");
        return;
    }
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Button ISR service failed — pairing token cannot be read");
        return;
    }
    if (gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Button ISR attach failed — pairing token cannot be read");
        return;
    }
    s_button_ready = true;
}

// A pin that failed to configure reads low, so the hold is only trusted once
// the button is set up. The hold is published for the page's "keep holding"
// card (wifi_prov_ap_reset_hold()); the reset itself waits one more second
// after the last number so that card can say it is resetting before the
// sidekick goes away.
static void button_check_hold(void)
{
    static int held;
    if (!s_button_ready || gpio_get_level(BUTTON_PIN) != 0) {
        held = 0;
        wifi_prov_set_ap_reset_hold(0);
        return;
    }
    ++held;
    wifi_prov_set_ap_reset_hold(held > BUTTON_HOLD_TICKS ? BUTTON_HOLD_TICKS : held);
    if (held >= BUTTON_HOLD_TICKS) {
        ESP_LOGW(TAG, "Button held -- resetting the AP name and password");
        vTaskDelay(pdMS_TO_TICKS(1500));
        wifi_prov_ap_reset_and_restart();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Micro-FT8 sidekick booting (IDF %s)", esp_get_idf_version());

    // Beacon first, WiFi second. The PORTA companion link is what the main board
    // depends on; a sidekick that cannot reach WiFi is degraded, not broken,
    // so nothing about network bring-up is allowed to delay or block it.
    porta_beacon_init();
    host_link_start();
    wifi_prov_start();
    // After wifi_prov_start(), which is what initialises NVS and mints the
    // token; a press before there is a token to disclose would do nothing.
    button_init();

    if (wifi_prov_is_online()) {
        ESP_LOGI(TAG, "WiFi: online on '%s'", wifi_prov_ssid());
    } else if (wifi_prov_ap_active()) {
        ESP_LOGW(TAG, "WiFi: provisioning AP up, not joined");
    }

    uint32_t heartbeat = 0;
    while (1) {
        if (heartbeat % BEACON_EVERY == 0) {
            porta_beacon_send();
        }
        mark_valid_once_beaconing(heartbeat);
        if (s_button_pressed) {
            s_button_pressed = false;
            pairing_http_open_disclosure();
        }
        button_check_hold();
        if (heartbeat % 5 == 0) {
            ESP_LOGI(TAG, "alive: %" PRIu32 " wifi=%s", heartbeat / 5,
                     wifi_prov_is_online() ? "up" : (wifi_prov_ap_active() ? "ap" : "down"));
        }
        ++heartbeat;
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
