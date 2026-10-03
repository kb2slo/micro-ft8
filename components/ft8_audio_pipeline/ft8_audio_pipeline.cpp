#include "ft8_audio_pipeline.h"
#include "ft8_sample_ring.h"
#include "ft8_slot_deficit.h"
#include "decode_tx_state.h"
#include "debug_log.h"

#include "protocol.h"

#include <cmath>
#include <cstdio>
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

// 2.7 s at 6 kHz. The 20 m run dropped about 2 s of audio while decode held
// the capture task; the ingest task fills this while that decode runs.
// Static: after the 161 KB waterfall the largest free block is about 51 KB,
// and this ring is 64 KB. Only the P4 links this component.
static constexpr unsigned kIngestRingSamples = 16384;
static float s_ring_storage[kIngestRingSamples];

struct AudioIngest {
    const ft8_audio_pipeline_config_t *cfg;
    Ft8SampleRing *ring;
    volatile bool stop;
    volatile bool done;
};

static void audio_ingest_task(void *arg)
{
    AudioIngest *in = static_cast<AudioIngest *>(arg);
    float tmp[512];
    while (!in->stop && !in->cfg->should_stop(in->cfg->ctx)) {
        int n = in->cfg->read(in->cfg->ctx, tmp, 512);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (n > 512) n = 512;
        ft8_sample_ring_write(in->ring, tmp, (unsigned)n);
    }
    in->done = true;
    vTaskDelete(nullptr);
}

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
    memset(&mon, 0, sizeof(mon));
    monitor_init(&mon, &mon_cfg);
    log_heap("AUDIO_PIPE_AFTER_MONITOR_INIT");
    if (mon.wf.mag == nullptr) {
        ESP_LOGE(tag, "monitor init failed nfft=%d freq_osr=%d", mon.nfft, mon_cfg.freq_osr);
        return;
    }
    ESP_LOGI(tag, "monitor nfft=%d freq_osr=%d waterfall=%d bytes",
             mon.nfft, mon.wf.freq_osr, mon.wf.max_blocks * mon.wf.block_stride);

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

    Ft8SampleRing ring;
    ft8_sample_ring_init(&ring, s_ring_storage, kIngestRingSamples);
    AudioIngest ingest = { cfg, &ring, false, false };
    // Higher priority than this task, same core, so a USB read completes
    // during decode instead of waiting out the search.
    if (xTaskCreatePinnedToCore(audio_ingest_task, "audio_ingest", 6144, &ingest, 5,
                                nullptr, xPortGetCoreID()) != pdPASS) {
        ESP_LOGE(tag, "audio ingest task create failed");
        free(ft8_buffer);
        free(temp_dec);
        monitor_free(&mon);
        return;
    }
    ESP_LOGI(tag, "audio ingest ring=%u samples", kIngestRingSamples);

    int ft8_buffer_idx = 0;
    TickType_t next_wake = xTaskGetTickCount();
    int slot_blocks = 0;
    int64_t slot_samples = 0;
    unsigned dropped_at_boundary = 0;
    int64_t slot_idx = rtc_now_ms() / slot_ms;
    int64_t slot_start_ms = slot_idx * slot_ms;
    (void)slot_start_ms;

    while (!cfg->should_stop(cfg->ctx)) {
        int samples_dec = (int)ft8_sample_ring_read(&ring, temp_dec, 512);
        if (samples_dec <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (samples_dec > 512) samples_dec = 512;

        for (int i = 0; i < samples_dec && !cfg->should_stop(cfg->ctx); i++) {
            ft8_buffer[ft8_buffer_idx++] = temp_dec[i];
            slot_samples++;

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
                // A backlog is audio captured while decode ran. Process it now;
                // pacing it out would hold it past the boundary.
                if (ft8_sample_ring_count(&ring) < (unsigned)mon.block_size) {
                    vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(sym_delay_ms));
                } else {
                    next_wake = xTaskGetTickCount();
                }

                slot_blocks++;
                int64_t now_idx = rtc_now_ms() / slot_ms;
                if (now_idx != slot_idx) {
                    const int64_t lost = ft8_slot_sample_deficit(
                        slot_samples, now_idx - slot_idx, (int)slot_ms, FT8_SAMPLE_RATE);
                    const unsigned dropped = ft8_sample_ring_dropped(&ring);
                    const unsigned drop = dropped - dropped_at_boundary;
                    dropped_at_boundary = dropped;
                    // g is blocks captured after the decode reset: about 14 is a full guard.
                    // Port A log, not the console UART: the sidekick prints it.
                    {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "BDRY %lld g%d lost%lld drop%u",
                                 (long long)slot_idx, slot_blocks, (long long)lost, drop);
                        debug_log_line(buf);
                    }
                    if (slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                        decode_tx_state_set_decode_applied_slot_idx(slot_idx);
                    }
                    if (slot_idx == decode_tx_state_get_tx_slot_idx()) {
                        decode_tx_state_set_tx_slot_idx(-1);
                    }
                    slot_idx = now_idx;
                    slot_start_ms = slot_idx * slot_ms;
                    slot_blocks = 0;
                    slot_samples = 0;
                    mon.wf.num_blocks = 0;
                    monitor_reset(&mon);
                    next_wake = xTaskGetTickCount();
                } else if (slot_blocks >= g_protocol->total_symbols &&
                           mon.wf.num_blocks >= g_protocol->total_symbols) {
                    ESP_LOGD(tag, "Triggering decode at slot %lld blocks=%d wf=%d",
                             (long long)slot_idx, slot_blocks, mon.wf.num_blocks);
                    // tx_slot_idx outlives was_txing. The slot loop clears that
                    // flag ~10 ms after the tones end, and the tones end at the
                    // 79th block, so the flag is often already false here.
                    if (decode_tx_state_get_tx_active() || decode_tx_state_get_was_txing() ||
                        slot_idx == decode_tx_state_get_tx_slot_idx()) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "SKIP %lld", (long long)slot_idx);
                        debug_log_line(buf);
                        if (slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                            decode_tx_state_set_decode_applied_slot_idx(slot_idx);
                        }
                        if (slot_idx == decode_tx_state_get_tx_slot_idx()) {
                            decode_tx_state_set_tx_slot_idx(-1);
                        }
                    } else if (decode_tx_state_get_decode_enabled()) {
                        decode_tx_state_set_decode_slot_idx(slot_idx);
                        decode_tx_state_set_decode_in_progress(true);
                        decode_monitor_results(&mon, &mon_cfg);
                    } else {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "PAUSED %lld", (long long)slot_idx);
                        debug_log_line(buf);
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

    ingest.stop = true;
    for (int i = 0; i < 50 && !ingest.done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(ft8_buffer);
    free(temp_dec);
    monitor_free(&mon);
}
