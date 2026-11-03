#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "debug_log.h"
#include "decode_tx_state.h"
#include "esp_heap_caps.h"
#include "porta.h"
#include "qmx_transport.h"
#include "cat_reply_parser.h"
#include "protocol.h"
#include "qmx_rx_mode.h"
#include "rtc_core.h"
#include "iq_frontend.h"
#include "radio_transport.h"
#include "resample.h"
#include "usb/cdc_acm_host.h"
#include "usb/uac_host.h"
#include "usb/usb_host.h"

// docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)): this board's implementation of the radio_transport.h
// seam components/radio_control needs (docs/STYLE.md (Two boards, one source)) --
// USB host, CDC-ACM, and UAC lifecycle are board-specific (this
// board has no presence-detection client or TX audio yet), so that part
// stays here while everything CAT-protocol-shaped lives in the shared
// component instead of being duplicated. Only the QMX backend is wired up
// (main.cpp selects RADIO_CONTROL_QMX); the QDX-only uac_tx_* functions
// below are honestly unimplemented, not silently faked -- see the comment
// on them. qmx_audio_* (RX only -- decode audio) is this file's other half,
// feeding components/ft8_audio_pipeline. There is no TX audio out or hot-plug
// presence detection yet.
//
// Task split: usb_lib_task owns
// usb_host_lib_handle_events() + CDC and must never block; uac_lib_task
// owns the UAC open/settle/start sequence (safe to block) and is woken by
// a queue from uac_driver_event_cb. Field-found 2026-09-18: running
// try_open_uac() on the events-pump task wedged the control pipe (SET_
// INTERFACE completion never drained) and crashed on teardown.

static const char *TAG = "qmx_transport";

static constexpr uint16_t kQmxVid = 0x0483;
static constexpr uint16_t kQmxPid = 0xA34C;

// QMX's fixed UAC format -- this board only ever talks to a QMX, so
// there's no generic-USB candidate list to try.
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

// What the browser has been told about the USB-C port (USB_STATUS), so a
// plug-in shows as Found when the host core first sees the device -- about
// 11 s before CDC opens, a QMX-side delay (docs/rfcs/0009-operate-station-
// surface.md, section 6, O2) -- and a device that never opens still clears when it
// goes. announced: an attach was sent and no detach since. announced_known:
// that attach carried the device's VID/PID. Written on usb_lib_task and the
// CDC event task, hence volatile.
static volatile bool s_usb_announced = false;
static volatile bool s_usb_announced_known = false;
static int64_t s_usb_first_seen_ms = 0;
// The address list can lag the descriptor callback; this long with nothing
// from the callback, say a device is there without naming it.
static constexpr int64_t kUsbUnidentifiedAfterMs = 1500;
// Port kind on the wire (porta_proto.h, UsbCKind): 0 = the QMX, 2 = other.
static constexpr uint8_t kUsbKindRadio = 0;
static constexpr uint8_t kUsbKindOther = 2;

// Heap across QMX reconnects (docs/architecture/memory.md): one Port A line per
// device departure, with the heap read once the USB host has dropped the
// device. The host core frees a device only after every client has closed it,
// so the empty address list comes after both the CDC close and the deferred UAC
// close; the settle lets the decode pipeline's own teardown on core 1 finish too.
// A reading at the pipeline's start also holds what was allocated on first use
// in the session before, so only this one compares cycle to cycle.
static constexpr int64_t kUsbGoneHeapSettleMs = 2000;
static bool s_usb_dev_listed = false;
static int64_t s_usb_gone_at_ms = 0;
static uint32_t s_usb_gone_count = 0;

static void log_heap_after_usb_gone(void) {
    char buf[96];
    snprintf(buf, sizeof(buf), "USBC gone n%lu int %u/%u ps %u/%u",
             (unsigned long)s_usb_gone_count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "%s", buf);
    debug_log_line_public(buf);
}

static uac_host_device_handle_t s_uac_handle = nullptr;
// QMX can fire DISCONNECTED once per UAC logic iface; only queue one close.
static uac_host_device_handle_t s_uac_close_queued = nullptr;

// UAC driver events arrive on
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

// The I/Q capture path (Station qmx_iq, qmx_iq_ab). One front end, one channel: FT8 at 13,500 Hz above the LO,
// I on the left channel, Q not negated, no IF trim, 6 kHz out (the 2026-11-02 corpus's settings,
// tools/decode_bench/corpus/qmx-2026-11-02). Internal RAM, .bss: sizeof(iq_frontend_t) plus the
// conversion buffer, checked by tools/check_ram_budget.py. Used only by the ingest task, which
// calls qmx_audio_read_ft8_samples(); initialised on the UAC task before a stream is published.
// Which path a read takes follows the plan (qmx_rx_mode.h), which the main loop writes and the ingest
// task reads: one 64-bit word, read whole. Correct, though not lock-free: on rv32 a 64-bit atomic goes
// through libatomic. s_iq_active is what the ingest task is converting now; s_plan_applied is its latch.
static constexpr double kIqChannelHz = 13500.0;
static std::atomic<uint64_t> s_rx_plan{0};
static bool s_iq_active;
static int64_t s_plan_applied = -1;      // ingest task: the plan slot it has applied
static bool s_change_pending;            // main loop: a change was made and its Q9 is not yet out
static std::atomic<int> s_ab_slots{0};   // qmx_iq_ab as the main loop last read it
static iq_frontend_t s_iq_frontend;
static float s_iq_frames[2 * (UAC_READ_BUFFER_SIZE / 6)];  // one read's frames, I then Q, ±1
static std::atomic<uint32_t> s_frontend_us;  // the front end's time, for BDRY's fe
static DMA_ATTR uint8_t s_usb_buffer[UAC_READ_BUFFER_SIZE];

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

void cat_cdc_reset_parsed_meters(void);  // defined with radio_transport CAT group below
static void forget_radio_version(void);  // likewise

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void * /*user_ctx*/) {
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "QMX CDC disconnected");
        // CDC only, not UAC too: they drop within the same real unplug event
        // (a few hundred ms apart, per bench log), and firing on both would
        // just double the banner for one physical event. kind is always 0
        // (QmxQdx) -- this board's USB-C port has exactly one occupant, ever,
        // so there is nothing to classify.
        porta_emit_usb_status(false, kUsbKindRadio, kQmxVid, kQmxPid);
        s_usb_announced = false;
        s_usb_announced_known = false;
        s_usb_first_seen_ms = 0;
        // Field-found 2026-09-17: cdc_acm_host_close() must be called even
        // though the device is already gone, or the driver's interface/
        // endpoint claim for this handle leaks. The next cdc_acm_host_open()
        // then fails to claim the interface (EP alloc / claim errors), and
        // repeated retries against that leaked state eventually hang
        // usb_lib_task inside descriptor parsing (watchdog reboot loop).
        // Null first so
        // cat_cdc_send() on another task cannot race into the same handle.
        cdc_acm_dev_hdl_t h = s_cdc_handle;
        s_cdc_handle = nullptr;
        s_cdc_reconnect_after_ms = now_ms() + kCdcReconnectBackoffMs;
        s_cdc_open_attempts = 0;
        s_cdc_first_attempt_ms = 0;
        cat_cdc_reset_parsed_meters();
        forget_radio_version();
        if (h) {
            cdc_acm_host_close(h);
        }
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGW(TAG, "QMX CDC error: %d", event->data.error);
    }
}

static cat_reply_parser_t make_cat_parser() {
    cat_reply_parser_t parser;
    cat_reply_parser_init(&parser);
    return parser;
}
static cat_reply_parser_t s_cat_parser = make_cat_parser();

static bool cdc_data_cb(const uint8_t *data, size_t data_len, void* /*user_arg*/) {
    if (!data || data_len == 0) {
        return true;
    }
    cat_reply_parser_feed(&s_cat_parser, data, data_len);
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
        const bool radio = desc->idVendor == kQmxVid && desc->idProduct == kQmxPid;
        porta_emit_usb_status(true, radio ? kUsbKindRadio : kUsbKindOther,
                              desc->idVendor, desc->idProduct);
        s_usb_announced = true;
        s_usb_announced_known = true;
    } else {
        ESP_LOGI(TAG, "B67: CDC client saw new USB dev at %lld ms (descriptor read failed: %s)",
                 (long long)now_ms(), esp_err_to_name(err));
    }
}

// Announces a device the host core can see but the CDC client has not named
// (the descriptor callback did not fire, or not yet), and clears an attach
// whose device left before CDC ever opened -- no CDC disconnect comes then.
// Runs every pass of usb_lib_task.
static void track_usb_presence(void) {
    uint8_t addrs[10];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) != ESP_OK) {
        return;
    }
    const int64_t t = now_ms();
    if (n > 0) {
        s_usb_dev_listed = true;
        s_usb_gone_at_ms = 0;
    } else if (s_usb_dev_listed) {
        s_usb_dev_listed = false;
        s_usb_gone_count++;
        s_usb_gone_at_ms = t;
    } else if (s_usb_gone_at_ms != 0 && t - s_usb_gone_at_ms >= kUsbGoneHeapSettleMs) {
        s_usb_gone_at_ms = 0;
        log_heap_after_usb_gone();
    }
    if (n > 0) {
        if (s_usb_announced || t < s_cdc_reconnect_after_ms) {
            return;  // told already, or a device that just left is still listed
        }
        if (s_usb_first_seen_ms == 0) {
            s_usb_first_seen_ms = t;
        } else if (t - s_usb_first_seen_ms >= kUsbUnidentifiedAfterMs) {
            porta_emit_usb_status(true, kUsbKindOther, 0, 0);
            s_usb_announced = true;
        }
        return;
    }
    s_usb_first_seen_ms = 0;
    if (s_usb_announced && !s_cdc_handle) {
        porta_emit_usb_status(false, kUsbKindOther, 0, 0);
        s_usb_announced = false;
        s_usb_announced_known = false;
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
    if (!s_usb_announced_known) {
        porta_emit_usb_status(true, kUsbKindRadio, kQmxVid, kQmxPid);
        s_usb_announced = true;
        s_usb_announced_known = true;
    }
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

// Field-found 2026-09-18: closing from this callback raced the reader
// (audio_ingest, core 0) still inside uac_host_device_read() and asserted in
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
// knows.
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
    {
        // Always, whatever the path now: qmx_iq can be switched on at the next slot boundary. A fresh
        // stream starts with cleared filters and the NCO at phase 0.
        const iq_frontend_config_t config = {
            .input_rate_hz = kUacSampleRate,
            .output_rate_hz = 6000,
            .center_hz = kIqChannelHz,
            .if_trim_hz = 0.0,
            .swap_iq = false,
            .negate_q = false,
        };
        const iq_frontend_status_t status = iq_frontend_init(&s_iq_frontend, &config);
        if (status != IQ_FRONTEND_OK) {
            ESP_LOGE(TAG, "I/Q front end init failed (%d): I/Q cannot be decoded, audio is unaffected",
                     (int)status);
        }
        // The init runs on this 4,096-byte task with soft-double libm, at every publish whatever the key.
        ESP_LOGI(TAG, "I/Q front end init: uac_lib stack high-water mark %u bytes free",
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
    // Both before the handle is published: a reader may start the moment it is.
    resample_init(&s_resample_state);
    s_uac_handle = handle;
    s_uac_starting_handle = nullptr;
    s_uac_close_queued = nullptr;
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

    // "QMX ISO split" FIFO tuning: this composite device (CDC CAT + UAC audio) needs both a CDC bulk-OUT
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
        track_usb_presence();
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
    cat_reply_parser_reset(&s_cat_parser);
}

bool cat_cdc_get_parsed_meters(int* pc_tenths, int* sw_hundredths) {
    const int pc = s_cat_parser.pc_tenths;
    const int sw = s_cat_parser.sw_hundredths;
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

bool cat_cdc_get_parsed_freq(int* hz) {
    const int value = s_cat_parser.fa_hz;
    if (value < 0) {
        return false;
    }
    if (hz) {
        *hz = value;
    }
    return true;
}

void cat_cdc_reset_parsed_if(void) {
    s_cat_parser.if_hz = -1;
    s_cat_parser.if_tx = -1;
    s_cat_parser.if_mode = -1;
    s_cat_parser.if_split = -1;
}

static void forget_radio_version(void) {
    cat_reply_parser_forget_version(&s_cat_parser);
}

bool cat_cdc_get_version(char* out, size_t cap) {
    if (s_cat_parser.vn[0] == 0 || !out || cap == 0) {
        return false;
    }
    snprintf(out, cap, "%s", s_cat_parser.vn);
    return true;
}

uint32_t cat_cdc_if_changes(void) {
    return s_cat_parser.if_changes;
}

bool cat_cdc_get_parsed_if(int* hz, int* tx, int* mode, int* split) {
    const int value = s_cat_parser.if_hz;
    if (value < 0) {
        return false;
    }
    if (hz) {
        *hz = value;
    }
    if (tx) {
        *tx = s_cat_parser.if_tx;
    }
    if (mode) {
        *mode = s_cat_parser.if_mode;
    }
    if (split) {
        *split = s_cat_parser.if_split;
    }
    return true;
}

bool cat_cdc_get_parsed_q9(int* iq) {
    const int value = s_cat_parser.q9_iq;
    if (value < 0) {
        return false;
    }
    if (iq) {
        *iq = value;
    }
    return true;
}

void cat_cdc_reset_parsed_q9(void) {
    s_cat_parser.q9_iq = -1;
}

bool cat_cdc_get_parsed_sr(int* lock) {
    const int value = s_cat_parser.sr_lock;
    if (value < 0) {
        return false;
    }
    if (lock) {
        *lock = value;
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

// The 24-bit stereo frames in s_usb_buffer as I/Q, through the front end into `out` at 6 kHz.
static int read_iq_samples(int frames, float *out, int max_samples) {
    static_assert(kUacBitResolution == 24 && kUacChannels == 2, "the I/Q path reads 24-bit stereo");
    if (frames <= 0 || frames > (int)(sizeof(s_iq_frames) / sizeof(s_iq_frames[0]) / 2)) {
        return 0;
    }
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 2 * frames; ++i) {
        const uint8_t *p = s_usb_buffer + 3 * i;
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) {
            v |= (int32_t)0xFF000000;
        }
        s_iq_frames[i] = (float)v / 8388608.0f;
    }
    const int n = iq_frontend_process(&s_iq_frontend, s_iq_frames, (size_t)frames, out,
                                      (size_t)max_samples);
    s_frontend_us.fetch_add((uint32_t)(esp_timer_get_time() - t0), std::memory_order_relaxed);
    return n > 0 ? n : 0;
}

int qmx_audio_read_ft8_samples(void * /*ctx*/, float *out, int max_samples) {
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

    // The path for this moment: the plan's, which changes at a slot boundary and not between. The
    // radio changed format 0.5 s earlier, in the idle tail of the slot before.
    const int64_t now = rtc_now_ms();
    const bool want_iq = qmx_rx_plan_iq_at(qmx_rx_plan_unpack(s_rx_plan.load(std::memory_order_relaxed)), now,
                                            g_protocol->slot_time_ms, &s_plan_applied);
    if (want_iq != s_iq_active) {
        s_iq_active = want_iq;
        if (want_iq) {
            iq_frontend_reset(&s_iq_frontend);  // no slot mixes paths: the front end starts empty
        } else {
            resample_init(&s_resample_state);   // and so does the audio resampler
        }
        char line[48];
        snprintf(line, sizeof(line), "RXPATH %lld %s +%lldms", (long long)(now / g_protocol->slot_time_ms),
                 want_iq ? "iq" : "au", (long long)(now % g_protocol->slot_time_ms));
        debug_log_line(line);
    }
    if (s_iq_active) {
        return read_iq_samples((int)bytes_read / frame_bytes, out, max_samples);
    }
    return uac_pcm_to_ft8_samples(&s_resample_state, s_usb_buffer, (int)bytes_read, out,
                                  kUacBitResolution, kUacChannels);
}

void qmx_audio_rx_init(bool qmx_iq, int ab_slots) {
    const int64_t slot = qmx_rx_slot_of(rtc_now_ms(), g_protocol->slot_time_ms);
    const bool iq = qmx_rx_slot_wants_iq(slot, qmx_iq, ab_slots);
    s_rx_plan.store(qmx_rx_plan_pack(qmx_rx_plan_make(slot, iq)));
    s_iq_active = iq;
    s_ab_slots.store(ab_slots);
    s_change_pending = false;
    ESP_LOGI(TAG, "QMX receive decodes from %s (qmx_iq=%d qmx_iq_ab=%d)", iq ? "I/Q" : "audio",
             qmx_iq ? 1 : 0, ab_slots);
}

// Takes back the change just made (qmx_rx_plan_cancel) and says so. Main loop only, which alone writes the plan.
static void rx_cancel(const char *why) {
    QmxRxPlan plan = qmx_rx_plan_unpack(s_rx_plan.load());
    qmx_rx_plan_cancel(&plan);
    s_rx_plan.store(qmx_rx_plan_pack(plan));
    s_change_pending = false;
    char line[48];
    snprintf(line, sizeof(line), "RXPATH %lld kept %s: Q9 %s", (long long)plan.slot, plan.iq ? "iq" : "au", why);
    debug_log_line(line);
}

bool qmx_audio_rx_tick(bool qmx_iq, int ab_slots, bool busy) {
    s_ab_slots.store(ab_slots);
    const int64_t now = rtc_now_ms();
    QmxRxPlan plan = qmx_rx_plan_unpack(s_rx_plan.load());
    if (s_change_pending && qmx_rx_plan_late(plan, now, g_protocol->slot_time_ms)) {
        rx_cancel("late");  // the main loop stalled past the boundary before sending
        plan = qmx_rx_plan_unpack(s_rx_plan.load());
    }
    const bool changed = qmx_rx_plan_step(&plan, now, g_protocol->slot_time_ms, qmx_iq, ab_slots, busy);
    s_rx_plan.store(qmx_rx_plan_pack(plan));
    if (changed) s_change_pending = true;
    return changed;
}

void qmx_audio_rx_sent(void) {
    if (!s_change_pending) return;
    const QmxRxPlan plan = qmx_rx_plan_unpack(s_rx_plan.load());
    if (qmx_rx_plan_late(plan, rtc_now_ms(), g_protocol->slot_time_ms)) {
        rx_cancel("sent after the boundary");
    } else {
        s_change_pending = false;
    }
}

void qmx_audio_rx_not_sent(void) {
    if (s_change_pending) rx_cancel("not delivered");
}

bool qmx_audio_rx_want_iq(void) {
    return qmx_rx_plan_unpack(s_rx_plan.load()).iq;
}

int qmx_audio_rx_path_for_slot(void * /*ctx*/, int64_t slot_idx) {
    if (s_ab_slots.load() == 0) return 0;  // no tag unless the experiment is on: the SLOT line stays as it was
    return qmx_rx_plan_iq_for_slot(qmx_rx_plan_unpack(s_rx_plan.load()), slot_idx) ? 2 : 1;
}

uint32_t qmx_audio_take_frontend_us(void * /*ctx*/) {
    return s_frontend_us.exchange(0, std::memory_order_relaxed);
}

bool qmx_audio_should_stop(void * /*ctx*/) {
    return s_uac_handle == nullptr;
}
