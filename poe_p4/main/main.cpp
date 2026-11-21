#include <atomic>
#include <climits>
#include <cstdio>
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
#include "hl2_transport.h"
#include "porta.h"
#include "porta_host.h"
#include "protocol.h"
#include "qmx_iq_channel.h"
#include "station.h"
#include "qmx_transport.h"
#include "radio_control.h"
#include "radio_select.h"
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

static_assert((int)FT8_SLOT_UNDECODED_TX == (int)PORTA_UNDECODED_TX &&
                  (int)FT8_SLOT_UNDECODED_BUSY == (int)PORTA_UNDECODED_BUSY &&
                  (int)FT8_SLOT_UNDECODED_PAUSED == (int)PORTA_UNDECODED_PAUSED,
              "why a slot was not decoded: the pipeline's values are the event's");

static void send_undecoded(void * /*ctx*/, int64_t slot_idx, int slot_ms, ft8_slot_undecoded_t why) {
    porta_emit_undecoded((uint32_t)slot_idx, (uint8_t)(slot_ms / 100), (uint8_t)why);
}

static void send_activity(void * /*ctx*/, int ft4, int64_t slot_idx, int block_ms, int first,
                          const uint8_t *levels, const uint8_t *masks, int n, int lo_hz, int hi_hz) {
    porta_emit_activity((uint8_t)(ft4 ? 1 : 0), (uint32_t)slot_idx, (uint8_t)block_ms, (uint8_t)first, levels,
                        masks, (uint8_t)n, (uint16_t)lo_hz, (uint16_t)hi_hz);
}

// Which channels the radio feeds (docs/architecture/decode.md, Channels): the operating band's plan
// (porta_p4_station_plan(), station_channel.h). FT8 alone, the station's channel in FT8; FT4 alone, the station's
// channel in FT4 at FT4's dial, on either radio; or FT8 and FT4 beside it, from a QMX's I/Q, FT4's dial this far
// from FT8's. The main loop works it out each pass; a run of the pipeline is built for one plan and ends when the
// plan changes.
static constexpr int32_t kNoFt4 = INT32_MIN;          // FT8 alone
static constexpr int32_t kFt4Alone = INT32_MIN + 1;   // FT4 alone, as the station's channel
static std::atomic<int32_t> s_rx_plan{kNoFt4};
static int32_t s_run_plan = kNoFt4;  // decode_audio task only

static void rx_plan_tick(void) {
    const StationPlan p = porta_p4_station_plan();
    int32_t plan = kNoFt4;
    if (p.listen == kListenFt4) {
        plan = kFt4Alone;
    } else if (p.listen == kListenBoth) {
        plan = p.ft4_offset_hz;
    }
    s_rx_plan.store(plan);
}

// A radio's samples feed decode only while it is the active radio (radio_select.h); choosing another radio ends
// the pipeline run, as a QMX unplugged does, and the task starts one on the radio now chosen. So does a change
// of channels: FT4 turned on or off, or a band where its channel sits elsewhere.
static bool qmx_decode_should_stop(void *ctx) {
    return qmx_audio_should_stop(ctx) || !radio_select_qmx_feeds_decode() || s_rx_plan.load() != s_run_plan;
}

static bool hl2_decode_should_stop(void *ctx) {
    return hl2_audio_should_stop(ctx) || !radio_select_hl2_feeds_decode() || s_rx_plan.load() != s_run_plan;
}

// The station's channel (docs/architecture/decode.md, Channels): the plan's first mode, at t2/f2.
// The QMX's and the HL2's run on it in turn, never together.
constexpr int kRxChannel = 0;
constexpr int kRxTimeOsr = 2;
constexpr int kRxFreqOsr = 2;

static void decode_audio_task(void * /*arg*/) {
    ft8_audio_channel_config_t station_channel = {
        .channel = kRxChannel,
        .mode = g_protocol,
        .time_osr = kRxTimeOsr,
        .freq_osr = kRxFreqOsr,
        .dial_offset_hz = 0,
        .station = true,
    };
    ft8_audio_pipeline_config_t hl2_cfg = {
        .tag = TAG,
        .ctx = nullptr,
        .read = hl2_audio_read_ft8_samples,
        .should_stop = hl2_decode_should_stop,
        .on_block_processed = nullptr,
        .on_activity = send_activity,
        .on_undecoded = send_undecoded,
        .take_frontend_us = hl2_audio_take_frontend_us,
        .n_channels = 1,
        .channels = {station_channel},
    };
    ft8_audio_pipeline_config_t pipe_cfg = {
        .tag = TAG,
        .ctx = nullptr,
        .read = qmx_audio_read_ft8_samples,
        .should_stop = qmx_decode_should_stop,
        .on_block_processed = nullptr,
        .on_activity = send_activity,
        .on_undecoded = send_undecoded,
        .take_frontend_us = qmx_audio_take_frontend_us,
        .n_channels = 1,
        .channels = {station_channel},
    };
    // QMX CAT/UAC comes back only after a radio power cycle (QMX quirk);
    // keep this task alive so a replug restarts decode without resetting P4.
    while (true) {
        const ft8_audio_pipeline_config_t *cfg = nullptr;
        while (!cfg) {
            // The station's channel in the plan's mode: FT4's when it listens to FT4 alone (rx_plan_tick()).
            s_run_plan = s_rx_plan.load();
            g_protocol = s_run_plan == kFt4Alone ? &kProtocolFT4 : &kProtocolFT8;
            station_channel.mode = g_protocol;
            pipe_cfg.channels[0] = station_channel;
            hl2_cfg.channels[0] = station_channel;
            if (radio_select_qmx_feeds_decode() && qmx_audio_ready()) {
                // FT4 beside FT8 if the plan has it.
                pipe_cfg.n_channels = 1;
                if (s_run_plan != kNoFt4 && s_run_plan != kFt4Alone) {
                    pipe_cfg.channels[1] = {
                        .channel = kRxChannel + 1,
                        .mode = &kProtocolFT4,
                        .time_osr = kRxTimeOsr,
                        .freq_osr = kRxFreqOsr,
                        .dial_offset_hz = s_run_plan,
                        .station = false,
                    };
                    pipe_cfg.n_channels = 2;
                    qmx_audio_set_ft4_dial_offset_hz(s_run_plan);
                    ESP_LOGI(TAG, "FT4 beside FT8, its dial %+ld Hz from FT8's", (long)s_run_plan);
                }
                cfg = &pipe_cfg;
                ESP_LOGI(TAG, "QMX UAC streaming; starting decode pipeline in %s", g_protocol->name);
            } else if (radio_select_hl2_feeds_decode() && hl2_audio_ready()) {
                cfg = &hl2_cfg;
                ESP_LOGI(TAG, "HL2 streaming; starting decode pipeline in %s", g_protocol->name);
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        porta_p4_set_decode_failed(false);
        if (!ft8_audio_pipeline_run(cfg)) {
            // Could not start (RAM, a task), so nothing will decode until it can. Say so once, on
            // the phone as well as here, and wait before trying again rather than every 60 ms.
            ESP_LOGE(TAG, "decode pipeline could not start: the radio is up but nothing will decode");
            debug_log_line("DECODE pipeline could not start: no RAM or task; retrying every 10 s");
            porta_p4_set_decode_failed(true);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        ESP_LOGW(TAG, "decode_audio_task: pipeline exited (the radio's stream stopped or another was chosen)");
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

    // Settings > Radio, applied again live on every change (radio_select.h).
    radio_select_apply(porta_p4_station_radio());
    porta_start();

    xTaskCreatePinnedToCore(decode_audio_task, "decode_audio", 8192, nullptr, 4, nullptr, 1);

    int ticks = 0;
    while (true) {
        porta_tick();
        porta_p4_companion_status_tick();
        porta_p4_tx_hud_tick();
        radio_control_dial_tick();
        porta_p4_radio_push_tick();
        porta_p4_file_tick();
        // The decode task hands each slot here: this loop alone touches autoseq
        // and the armed TX (docs/architecture/main_board.md, P4).
        decode_pipeline_apply();
        rx_plan_tick();
        tx_scheduler_tick();
        amp_band_prime_tick(esp_timer_get_time() / 1000);
        // A sweep request accepted just before a TX started waits for it to end; one already
        // running is always ticked, so it can de-key.
        if (!decode_tx_state_get_tx_active() || swr_sweep_active()) {
            swr_sweep_tick(esp_timer_get_time() / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        // IQ mode at each CAT link-up, read back (architecture/radio.md, The QMX's receive output).
        const bool rx_busy = decode_tx_state_get_tx_active() || porta_p4_tune_active() ||
                             amp_band_prime_busy() || swr_sweep_busy();
        switch (radio_control_rx_path_tick(rx_busy)) {
            case RADIO_RX_PATH_MISMATCH:
                debug_log_line("Q9 reads audio, not IQ: nothing will decode");
                break;
            case RADIO_RX_PATH_NO_REPLY:
                debug_log_line("Q9 unanswered: the QMX firmware may lack IQ mode; nothing will decode");
                break;
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
