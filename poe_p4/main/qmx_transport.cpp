#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "decode_tx_state.h"
#include "porta.h"
#include "qmx_transport.h"
#include "radio_transport.h"
#include "resample.h"
#include "usb/cdc_acm_host.h"
#include "usb/uac_host.h"
#include "usb/usb_host.h"

// docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)): this board's implementation of the radio_transport.h
// seam components/radio_control needs (docs/STYLE.md (Two boards, one source)) --
// USB host, CDC-ACM, and UAC lifecycle are board-specific (the ADV shares a
// port with a UAC audio pipeline and its own presence-detection client; this
// board has neither the presence client nor TX audio yet), so that part
// stays here while everything CAT-protocol-shaped lives in the shared
// component instead of being duplicated. Only the QMX backend is wired up
// (main.cpp selects RADIO_CONTROL_QMX); the QDX-only uac_tx_* functions
// below are honestly unimplemented, not silently faked -- see the comment
// on them. qmx_audio_* (RX only -- decode audio) is this file's other half,
// feeding components/ft8_audio_pipeline the same way ADV's stream_uac.cpp
// does, minus that file's TX-audio-out and hot-plug presence-detection
// pieces this board doesn't have yet.
//
// Task split mirrors ADV's stream_uac.cpp: usb_lib_task owns
// usb_host_lib_handle_events() + CDC and must never block; uac_lib_task
// owns the UAC open/settle/start sequence (safe to block) and is woken by
// a queue from uac_driver_event_cb. Field-found 2026-09-18: running
// try_open_uac() on the events-pump task wedged the control pipe (SET_
// INTERFACE completion never drained) and crashed on teardown.

static const char *TAG = "qmx_transport";

static constexpr uint16_t kQmxVid = 0x0483;
static constexpr uint16_t kQmxPid = 0xA34C;

// QMX's fixed UAC format -- same constants ADV's stream_uac.cpp uses for
// UAC_PROFILE_QMX (this board only ever talks to a QMX, so there's no
// generic-USB candidate list to try).
static constexpr uint32_t kUacSampleRate = 48000;
static constexpr uint8_t kUacBitResolution = 24;
static constexpr uint8_t kUacChannels = 2;
static constexpr uint32_t kUacBufferSize = 4000;
static constexpr uint32_t kUacBufferThreshold = 600;
#define UAC_READ_BUFFER_SIZE 2304

static cdc_acm_dev_hdl_t s_cdc_handle = nullptr;
static int64_t s_last_open_attempt_ms = 0;
// Field-found 2026-09-19: try_open()'s failure path was completely silent,
// so "the initial USB connection takes several seconds" had no evidence
// behind it -- not how many 1s-spaced attempts it took, not why they failed,
// not how long after the cable went in the first attempt even happened.
// These two plus the ESP_LOGD/ESP_LOGI below turn the next bench capture
// into a number instead of another guess.
static uint32_t s_cdc_open_attempts = 0;
static int64_t s_cdc_first_attempt_ms = 0;
// After CDC/UAC disconnect the composite device stays in usbh as is_gone for
// a while; cdc_acm_host_open() then spam-logs usbh_devs_open INVALID_STATE
// for its whole connection_timeout_ms. Hold off until the QMX is power-cycled
// (expected QMX CAT behaviour) and re-enumerates cleanly.
static constexpr int64_t kCdcReconnectBackoffMs = 3000;
static int64_t s_cdc_reconnect_after_ms = 0;

static uac_host_device_handle_t s_uac_handle = nullptr;
// QMX can fire DISCONNECTED once per UAC logic iface; only queue one close.
static uac_host_device_handle_t s_uac_close_queued = nullptr;

// Same shape as ADV's stream_uac.cpp uac_event_t — driver events arrive on
// the UAC driver's background task and must not do blocking open/start /
// close work there either; the queue hands them to uac_lib_task.
typedef enum {
    UAC_EVT_DRIVER,
    UAC_EVT_CLOSE,
} uac_event_type_t;

typedef struct {
    uac_event_type_t type;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
        } driver;
        uac_host_device_handle_t close_handle;
    };
} uac_event_t;

static QueueHandle_t s_uac_event_queue = nullptr;

// Field-found 2026-09-17: calling uac_host_device_start() immediately after
// uac_host_device_open() succeeds hit a USB control-transfer timeout during
// the interface's alt-setting switch (SET_INTERFACE). try_open_uac() settles
// before the first attempt and retries with a delay rather than failing once.
// That sequence blocks — which is why it lives on uac_lib_task, not on
// usb_lib_task (see file-level comment). The settle/retry alone does not
// clear the crash if the events pump is starved; the task split does.
static constexpr int64_t kUacStartSettleMs = 400;
static constexpr int kUacStartMaxAttempts = 8;

static resample_state_t s_resample_state;
static DMA_ATTR uint8_t s_usb_buffer[UAC_READ_BUFFER_SIZE];

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

void cat_cdc_reset_parsed_meters(void);  // defined with radio_transport CAT group below

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void * /*user_ctx*/) {
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "QMX CDC disconnected");
        // CDC only, not UAC too: they drop within the same real unplug event
        // (a few hundred ms apart, per bench log), and firing on both would
        // just double the banner for one physical event. kind is always 0
        // (QmxQdx) -- unlike the ADV, this board's USB-C port has exactly
        // one occupant, ever (porta_board.cpp: "P4 does not embed a sidekick
        // image (no USB-C flasher path like the ADV)"), so there is nothing
        // to classify.
        porta_emit_usb_status(false, 0, kQmxVid, kQmxPid);
        // Field-found 2026-09-17: cdc_acm_host_close() must be called even
        // though the device is already gone, or the driver's interface/
        // endpoint claim for this handle leaks. The next cdc_acm_host_open()
        // then fails to claim the interface (EP alloc / claim errors), and
        // repeated retries against that leaked state eventually hang
        // usb_lib_task inside descriptor parsing (watchdog reboot loop).
        // Matches the ADV's own cdc_close() in main/audio/stream_uac.cpp,
        // which already does this unconditionally. Null first so
        // cat_cdc_send() on another task cannot race into the same handle.
        cdc_acm_dev_hdl_t h = s_cdc_handle;
        s_cdc_handle = nullptr;
        s_cdc_reconnect_after_ms = now_ms() + kCdcReconnectBackoffMs;
        s_cdc_open_attempts = 0;
        s_cdc_first_attempt_ms = 0;
        cat_cdc_reset_parsed_meters();
        if (h) {
            cdc_acm_host_close(h);
        }
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGW(TAG, "QMX CDC error: %d", event->data.error);
    }
}

static volatile int s_pc_tenths = -1;
static volatile int s_sw_hundredths = -1;
static char s_cat_acc[48];
static size_t s_cat_acc_len = 0;

static void cat_parse_frame(const char *frame) {
    if (!frame || frame[0] == 0) {
        return;
    }
    if (frame[0] == 'P' && frame[1] == 'C' && frame[2] >= '0' && frame[2] <= '9') {
        int v = 0;
        for (const char *p = frame + 2; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + (*p - '0');
        }
        s_pc_tenths = v;
        return;
    }
    if (frame[0] == 'S' && frame[1] == 'W' && frame[2] >= '0' && frame[2] <= '9') {
        int v = 0;
        for (const char *p = frame + 2; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + (*p - '0');
        }
        s_sw_hundredths = v;
    }
}

static bool cdc_data_cb(const uint8_t *data, size_t data_len, void * /*user_arg*/) {
    if (!data || data_len == 0) {
        return true;
    }
    for (size_t i = 0; i < data_len; ++i) {
        const char c = static_cast<char>(data[i]);
        if (c == ';') {
            s_cat_acc[s_cat_acc_len] = 0;
            cat_parse_frame(s_cat_acc);
            s_cat_acc_len = 0;
            continue;
        }
        if (c < 32 || c > 126) {
            continue;
        }
        if (s_cat_acc_len + 1 < sizeof(s_cat_acc)) {
            s_cat_acc[s_cat_acc_len++] = c;
        } else {
            s_cat_acc_len = 0;
        }
    }
    return true;
}

// B67: fires the instant the CDC-ACM client sees USB_HOST_CLIENT_EVENT_NEW_DEV
// -- i.e. when the USB host core itself notices *any* device, before VID/PID
// matching or try_open()'s 1Hz polling ever runs. cdc_acm_find_and_open_usb_
// device() (vendored, cdc_acm_host.c) only ever polls usb_host_device_addr_
// list_fill(), a list the host core populates on its own; if try_open()'s
// ~11s delay is upstream of this file entirely, this line will land close to
// USB Host installed and try_open()'s attempt-1 will still be the slow one --
// if it lands ~11s in instead, that's the host core's own enumeration being
// slow, not anything in try_open()'s retry loop.
static void cdc_new_dev_cb(usb_device_handle_t usb_dev) {
    const usb_device_desc_t *desc = nullptr;
    esp_err_t err = usb_host_get_device_descriptor(usb_dev, &desc);
    if (err == ESP_OK && desc) {
        ESP_LOGI(TAG, "B67: CDC client saw new USB dev at %lld ms (VID:0x%04x PID:0x%04x)",
                 (long long)now_ms(), desc->idVendor, desc->idProduct);
    } else {
        ESP_LOGI(TAG, "B67: CDC client saw new USB dev at %lld ms (descriptor read failed: %s)",
                 (long long)now_ms(), esp_err_to_name(err));
    }
}

static void try_open(void) {
    if (s_cdc_handle) {
        return;
    }
    int64_t t = now_ms();
    if (t < s_cdc_reconnect_after_ms) {
        return;
    }
    if (s_last_open_attempt_ms != 0 && (t - s_last_open_attempt_ms) < 1000) {
        return;
    }
    s_last_open_attempt_ms = t;
    if (s_cdc_open_attempts == 0) {
        s_cdc_first_attempt_ms = t;
    }
    s_cdc_open_attempts++;

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
        // ESP_LOGD is compiled out at this build's CONFIG_LOG_MAXIMUM_LEVEL=3,
        // so this has to be LOGI to actually survive into a capture -- but
        // try_open() runs this path roughly once a second any time the QMX
        // is not connected at all, which is the common idle case (nothing
        // plugged in yet, or not for the rest of the session). Logging every
        // attempt would spam the log for as long as that lasts. First
        // attempt of a fresh streak always logs; after that, only every
        // 10th -- enough to show progress through a genuinely slow
        // connection without flooding an ordinary "not plugged in" wait.
        if (s_cdc_open_attempts == 1 || s_cdc_open_attempts % 10 == 0) {
            ESP_LOGI(TAG, "QMX CDC open attempt %lu failed: %s",
                     (unsigned long)s_cdc_open_attempts, esp_err_to_name(err));
        }
        return;  // not attached yet -- keep polling
    }
    s_cdc_handle = handle;
    ESP_LOGI(TAG, "QMX CDC-ACM opened (VID:0x%04x PID:0x%04x iface 0) -- %lu attempt(s), %lld ms since first",
             kQmxVid, kQmxPid, (unsigned long)s_cdc_open_attempts,
             (long long)(t - s_cdc_first_attempt_ms));
    s_cdc_open_attempts = 0;
    s_cdc_first_attempt_ms = 0;
    porta_emit_usb_status(true, 0, kQmxVid, kQmxPid);
    // The main loop pushes the Station band to the VFO once CAT is up. Every
    // open, not just the first: the QMX comes back only after a power cycle,
    // and it powers up on its own frequency.
    decode_tx_state_set_cdc_initial_sync_pending(true);
}

// Set for the duration of try_open_uac()'s blocking settle-and-retry loop
// below, so uac_device_event_cb() -- which can fire from the UAC driver's own
// background task, concurrently with that loop -- can tell it the handle it's
// about to retry uac_host_device_start() on has already been closed out from
// under it, rather than the loop calling start() again on a stale handle.
static volatile uac_host_device_handle_t s_uac_starting_handle = nullptr;

// Field-found 2026-09-18: closing from this callback raced decode_audio
// (core 1) still inside uac_host_device_read() and asserted in
// spinlock_acquire after QMX power-off. Espressif's audio_player example
// and the UAC README both require a user close on DISCONNECTED, but the
// example queues it off the driver task: null the published handle first
// so readers stop, then close on uac_lib_task.
static void uac_device_event_cb(uac_host_device_handle_t handle,
                                const uac_host_device_event_t event, void * /*arg*/) {
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        // Composite QMX can deliver DISCONNECTED more than once (one per
        // UAC logic iface). First wins; further closes would hit
        // ESP_ERR_INVALID_ARG after the iface is gone.
        if (handle == s_uac_close_queued) {
            return;
        }
        ESP_LOGI(TAG, "QMX UAC disconnected");
        if (handle == s_uac_handle) {
            s_uac_handle = nullptr;
        }
        if (handle == s_uac_starting_handle) {
            s_uac_starting_handle = nullptr;
        }
        s_uac_close_queued = handle;
        s_cdc_reconnect_after_ms = now_ms() + kCdcReconnectBackoffMs;
        if (s_uac_event_queue) {
            uac_event_t evt = {};
            evt.type = UAC_EVT_CLOSE;
            evt.close_handle = handle;
            if (xQueueSend(s_uac_event_queue, &evt, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGE(TAG, "UAC close event dropped; closing inline");
                uac_host_device_close(handle);
                s_uac_close_queued = nullptr;
            }
        } else {
            uac_host_device_close(handle);
            s_uac_close_queued = nullptr;
        }
    } else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "QMX UAC transfer error");
    }
}

// Queue for uac_lib_task. Only RX (microphone) interfaces fire RX_CONNECTED;
// a QMX's TX-capable interface (if it advertises one) would fire TX_CONNECTED
// instead and is ignored here -- this board doesn't use UAC TX (see uac_tx_*
// below). uac_host_device_open_with_vid_pid() looked like the simpler match
// to try_open()'s CDC polling style, but its iface_num isn't a wildcard --
// the library rejects 0 outright ("Invalid interface number"), so it still
// needs the real interface number, which only the driver's own enumeration
// knows. Same event-driven open ADV's stream_uac.cpp already uses.
static void uac_driver_event_cb(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event, void * /*arg*/) {
    ESP_LOGI(TAG, "UAC driver event %d addr=%d iface=%d", (int)event, addr, iface_num);
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED && !s_uac_handle && s_uac_event_queue) {
        uac_event_t evt = {};
        evt.type = UAC_EVT_DRIVER;
        evt.driver.addr = addr;
        evt.driver.iface_num = iface_num;
        evt.driver.event = event;
        xQueueSend(s_uac_event_queue, &evt, 0);
    }
}

// Runs on uac_lib_task only. Blocks that task for up to
// kUacStartSettleMs*kUacStartMaxAttempts while usb_lib_task keeps pumping
// usb_host_lib_handle_events() — required so SET_INTERFACE completions are
// drained instead of timing out.
static void try_open_uac(uint8_t addr, uint8_t iface_num) {
    if (s_uac_handle) {
        return;
    }

    const uac_host_device_config_t dev_cfg = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = kUacBufferSize,
        .buffer_threshold = kUacBufferThreshold,
        .callback = uac_device_event_cb,
        .callback_arg = nullptr,
    };
    uac_host_device_handle_t handle = nullptr;
    esp_err_t err = uac_host_device_open(&dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMX UAC open (addr=%d iface=%d) failed: %s",
                 addr, iface_num, esp_err_to_name(err));
        return;
    }
    s_uac_starting_handle = handle;

    const uac_host_stream_config_t stream_cfg = {
        .channels = kUacChannels,
        .bit_resolution = kUacBitResolution,
        .sample_freq = kUacSampleRate,
        .flags = 0,
    };

    esp_err_t start_err = ESP_FAIL;
    for (int attempt = 1; attempt <= kUacStartMaxAttempts; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(kUacStartSettleMs));
        if (!s_uac_starting_handle) {
            // Disconnected mid-settle (uac_device_event_cb, another task) --
            // it already closed the handle; nothing left to start or close.
            ESP_LOGW(TAG, "QMX UAC disconnected while settling; abandoning start");
            return;
        }
        start_err = uac_host_device_start(handle, &stream_cfg);
        if (start_err == ESP_OK) break;
        ESP_LOGW(TAG, "QMX UAC stream start attempt %d/%d failed: %s",
                 attempt, kUacStartMaxAttempts, esp_err_to_name(start_err));
    }

    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "QMX UAC giving up after %d attempts; replug to retry", kUacStartMaxAttempts);
        // Disconnect may already have queued UAC_EVT_CLOSE and cleared
        // s_uac_starting_handle — do not double-close.
        if (s_uac_starting_handle == handle) {
            s_uac_starting_handle = nullptr;
            uac_host_device_close(handle);
        }
        return;
    }

    if (!s_uac_starting_handle) {
        // Disconnected after a successful start() but before we published
        // s_uac_handle; UAC_EVT_CLOSE owns the close.
        ESP_LOGW(TAG, "QMX UAC disconnected after start; abandoning publish");
        return;
    }
    s_uac_handle = handle;
    s_uac_starting_handle = nullptr;
    s_uac_close_queued = nullptr;
    resample_init(&s_resample_state);
    ESP_LOGI(TAG, "QMX UAC RX streaming (%luHz %u-bit %uch)",
             (unsigned long)kUacSampleRate, (unsigned)kUacBitResolution, (unsigned)kUacChannels);
}

static void uac_lib_task(void * /*arg*/) {
    uac_event_t evt;
    while (true) {
        if (!xQueueReceive(s_uac_event_queue, &evt, portMAX_DELAY)) {
            continue;
        }
        if (evt.type == UAC_EVT_CLOSE) {
            // Give decode_audio one read-timeout window to observe the
            // nulled s_uac_handle / should_stop before ringbuf teardown.
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_err_t cerr = uac_host_device_close(evt.close_handle);
            // INVALID_ARG = iface already gone (duplicate DISCONNECTED or
            // driver finished teardown); not actionable.
            if (cerr != ESP_OK && cerr != ESP_ERR_INVALID_ARG) {
                ESP_LOGW(TAG, "QMX UAC close after disconnect: %s", esp_err_to_name(cerr));
            }
            if (s_uac_close_queued == evt.close_handle) {
                s_uac_close_queued = nullptr;
            }
            continue;
        }
        if (evt.type != UAC_EVT_DRIVER) {
            continue;
        }
        if (evt.driver.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
            if (s_uac_handle != nullptr) {
                ESP_LOGW(TAG, "Already have a UAC device, ignoring");
                continue;
            }
            ESP_LOGI(TAG, "Microphone connected - addr:%d, iface:%d",
                     evt.driver.addr, evt.driver.iface_num);
            try_open_uac(evt.driver.addr, evt.driver.iface_num);
        }
    }
}

static void usb_lib_task(void * /*arg*/) {
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;

    // Same "QMX ISO split" FIFO tuning as ADV's stream_uac.cpp usb_lib_task:
    // this composite device (CDC CAT + UAC audio) needs both a CDC bulk-OUT
    // allowance and UAC isochronous IN bandwidth from one usb_host_install(),
    // not two.
    host_config.fifo_settings_custom.rx_fifo_lines   = 91;
    host_config.fifo_settings_custom.nptx_fifo_lines = 18;
    host_config.fifo_settings_custom.ptx_fifo_lines  = 91;

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
        .new_dev_cb = cdc_new_dev_cb,
    };
    err = cdc_acm_host_install(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM driver installed");

    const uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_event_cb,
        .callback_arg = nullptr,
    };
    err = uac_host_install(&uac_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UAC driver installed");

    // B67: new_dev_cb (above) never fired in a bench capture where try_open()
    // still took ~11s/12 attempts to succeed -- so probe the same primitive
    // cdc_acm_find_and_open_usb_device() itself polls (usb_host_device_addr_
    // list_fill(), vendored cdc_acm_host.c) directly, right here, bypassing
    // the CDC driver layer and its possibly-unreliable client-event path
    // entirely. Logs once, the instant the raw device address list stops
    // being empty -- the earliest any client-side code could possibly see
    // this device, full stop.
    bool logged_first_addr = false;

    // Never block in this loop — UAC open/settle/start lives on uac_lib_task.
    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (!logged_first_addr) {
            uint8_t dev_addr_list[10];
            int num_of_devices = 0;
            if (usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_of_devices) == ESP_OK &&
                num_of_devices > 0) {
                ESP_LOGI(TAG, "B67: usb_host device addr list non-empty at %lld ms (%d device(s), first addr=%u)",
                         (long long)now_ms(), num_of_devices, (unsigned)dev_addr_list[0]);
                logged_first_addr = true;
            }
        }
        try_open();
    }
}

void qmx_transport_start(void) {
    s_uac_event_queue = xQueueCreate(10, sizeof(uac_event_t));
    if (!s_uac_event_queue) {
        ESP_LOGE(TAG, "Failed to create UAC event queue");
        return;
    }

    // uac_lib first so the queue consumer exists before usb_lib installs the
    // UAC driver and can produce RX_CONNECTED events.
    BaseType_t ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_lib", 4096, nullptr, 5, nullptr, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create uac_lib task");
        vQueueDelete(s_uac_event_queue);
        s_uac_event_queue = nullptr;
        return;
    }

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
    s_pc_tenths = -1;
    s_sw_hundredths = -1;
    s_cat_acc_len = 0;
}

bool cat_cdc_get_parsed_meters(int *pc_tenths, int *sw_hundredths) {
    const int pc = s_pc_tenths;
    const int sw = s_sw_hundredths;
    if (pc < 0 && sw < 0) {
        return false;
    }
    if (pc_tenths) {
        *pc_tenths = pc;
    }
    if (sw_hundredths) {
        *sw_hundredths = sw;
    }
    return true;
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

// qmx_audio_* -- RX decode audio, ft8_audio_pipeline_config_t's callbacks.

bool qmx_audio_ready(void) {
    return s_uac_handle != nullptr;
}

int qmx_audio_read_ft8_samples(void * /*ctx*/, float *out, int /*max_samples*/) {
    // Snapshot: disconnect nulls s_uac_handle before queuing close; a
    // concurrent close must not race a read that still holds the old
    // pointer only if we re-load it after the null. Local copy is enough
    // for the should_stop / ready handshake; close is deferred 250ms.
    uac_host_device_handle_t handle = s_uac_handle;
    if (!handle || !out) {
        return 0;
    }

    uint32_t bytes_read = 0;
    esp_err_t err = uac_host_device_read(handle, s_usb_buffer, UAC_READ_BUFFER_SIZE,
                                         &bytes_read, pdMS_TO_TICKS(200));
    if (err != ESP_OK || bytes_read == 0) {
        return 0;
    }

    const int frame_bytes = (kUacBitResolution / 8) * kUacChannels;
    if ((int)bytes_read < frame_bytes) {
        return 0;
    }

    return uac_pcm_to_ft8_samples(&s_resample_state, s_usb_buffer, (int)bytes_read, out,
                                  kUacBitResolution, kUacChannels);
}

bool qmx_audio_should_stop(void * /*ctx*/) {
    return s_uac_handle == nullptr;
}
