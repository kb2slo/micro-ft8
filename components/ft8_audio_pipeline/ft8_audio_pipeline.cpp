#include "ft8_audio_pipeline.h"
#include "decode_tx_state.h"
#include "debug_log.h"

#include "protocol.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

extern "C" {
#include "common/monitor.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
}

void decode_monitor_results(monitor_t* mon, const monitor_config_t* cfg);
int64_t rtc_now_ms();

#ifndef FT8_SAMPLE_RATE
#define FT8_SAMPLE_RATE 6000
#endif

void ft8_audio_pipeline_run(const ft8_audio_pipeline_config_t* cfg)
{
    if (!cfg || !cfg->read || !cfg->should_stop) return;
    const char* tag = cfg->tag ? cfg->tag : "FT8_AUDIO";

    monitor_config_t mon_cfg = {
        .f_min = 200.0f,
        .f_max = 2900.0f,
        .sample_rate = FT8_SAMPLE_RATE,
        .time_osr = decode_tx_state_get_time_osr(),
        .freq_osr = decode_tx_state_get_freq_osr(),
        .protocol = g_protocol->protocol_id
    };

    log_heap("AUDIO_PIPE_BEFORE_MONITOR_INIT");
    monitor_t mon;
    monitor_init(&mon, &mon_cfg);
    log_heap("AUDIO_PIPE_AFTER_MONITOR_INIT");

    monitor_reset(&mon);

    float* ft8_buffer = (float*)heap_caps_malloc(sizeof(float) * mon.block_size, MALLOC_CAP_DEFAULT);
    float* temp_dec = (float*)heap_caps_malloc(sizeof(float) * 512, MALLOC_CAP_DEFAULT);
    log_heap("AUDIO_PIPE_AFTER_SAMPLE_BUFFERS");

    if (!ft8_buffer || !temp_dec) {
        ESP_LOGE(tag, "pipeline buffer allocation failed");
        if (ft8_buffer) free(ft8_buffer);
        if (temp_dec) free(temp_dec);
        monitor_free(&mon);
        return;
    }

    const int64_t slot_ms      = g_protocol->slot_time_ms;
    const int     target_blocks = g_protocol->total_symbols + 1;
    const uint32_t sym_delay_ms = (uint32_t)(g_protocol->symbol_period * 1000.0f + 0.5f);

    int64_t now_ms = rtc_now_ms();
    int64_t rem = now_ms % slot_ms;
    int64_t wait_ms = (rem < 100) ? 0 : (slot_ms - rem);
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
    }

    int ft8_buffer_idx = 0;
    TickType_t next_wake = xTaskGetTickCount();
    int slot_blocks = 0;
    int64_t slot_idx = rtc_now_ms() / slot_ms;
    int64_t slot_start_ms = slot_idx * slot_ms;
    (void)slot_start_ms;

    while (!cfg->should_stop(cfg->ctx)) {
        int samples_dec = cfg->read(cfg->ctx, temp_dec, 512);
        if (samples_dec <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (samples_dec > 512) samples_dec = 512;

        for (int i = 0; i < samples_dec && !cfg->should_stop(cfg->ctx); i++) {
            ft8_buffer[ft8_buffer_idx++] = temp_dec[i];

            if (ft8_buffer_idx >= mon.block_size) {
                double acc = 0.0;
                for (int j = 0; j < mon.block_size; ++j) {
                    acc += fabsf(ft8_buffer[j]);
                }
                float level = (float)(acc / mon.block_size);
                float gain = (level > 1e-6f) ? 0.1f / level : 1.0f;
                if (gain < 0.1f) gain = 0.1f;
                if (gain > 10.0f) gain = 10.0f;
                for (int j = 0; j < mon.block_size; ++j) {
                    ft8_buffer[j] *= gain;
                }

                if (mon.wf.num_blocks < target_blocks) {
                    monitor_process(&mon, ft8_buffer);
                }

                if (cfg->on_block_processed) {
                    cfg->on_block_processed(cfg->ctx);
                }

                ft8_buffer_idx = 0;
                vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(sym_delay_ms));

                slot_blocks++;
                int64_t now_idx = rtc_now_ms() / slot_ms;
                if (now_idx != slot_idx) {
                    ESP_LOGD(tag, "Slot boundary %lld->%lld blocks=%d wf=%d",
                             (long long)slot_idx, (long long)now_idx,
                             slot_blocks, mon.wf.num_blocks);
                    if (slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                        decode_tx_state_set_decode_applied_slot_idx(slot_idx);
                    }
                    slot_idx = now_idx;
                    slot_start_ms = slot_idx * slot_ms;
                    slot_blocks = 0;
                    mon.wf.num_blocks = 0;
                    monitor_reset(&mon);
                    next_wake = xTaskGetTickCount();
                } else if (slot_blocks >= g_protocol->total_symbols &&
                           mon.wf.num_blocks >= g_protocol->total_symbols) {
                    ESP_LOGD(tag, "Triggering decode at slot %lld blocks=%d wf=%d",
                             (long long)slot_idx, slot_blocks, mon.wf.num_blocks);
                    if (decode_tx_state_get_tx_active() || decode_tx_state_get_was_txing()) {
                        ESP_LOGD(tag, "Skipping decode on TX slot %lld",
                                 (long long)slot_idx);
                        if (slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                            decode_tx_state_set_decode_applied_slot_idx(slot_idx);
                        }
                    } else if (decode_tx_state_get_decode_enabled()) {
                        decode_tx_state_set_decode_slot_idx(slot_idx);
                        decode_tx_state_set_decode_in_progress(true);
                        decode_monitor_results(&mon, &mon_cfg);
                    } else {
                        ESP_LOGD(tag, "Decode paused; skipping");
                        if (slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                            decode_tx_state_set_decode_applied_slot_idx(slot_idx);
                        }
                    }
                    monitor_reset(&mon);
                    mon.wf.num_blocks = 0;
                    slot_blocks = 0;
                    next_wake = xTaskGetTickCount();
                }
            }
        }
    }

    free(ft8_buffer);
    free(temp_dec);
    monitor_free(&mon);
}
