#include "ft8_audio_pipeline.h"
#include "ft8_sample_ring.h"
#include "ft8_slot_deficit.h"
#include "ft8_block_activity.h"
#include "decode_tx_state.h"
#include "debug_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

extern "C" {
#include "common/decode_psram.h"
#include "common/monitor.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
}

// Defined by decode_pipeline (decode_pipeline.h), which this component does not depend on.
void decode_slot(const ft8_slot_t* slot);
int64_t rtc_now_ms();

#ifndef FT8_SAMPLE_RATE
#define FT8_SAMPLE_RATE 6000
#endif

// 2.7 s at 6 kHz. The 20 m run dropped about 2 s of audio while decode held
// the capture task; the ingest task fills this while that decode runs.
static constexpr unsigned kIngestRingSamples = 16384;

// One channel's capture buffers, sized for the largest configuration a channel runs: FT8 at t2/f2,
// 93 blocks x 2 x 2 x 433 bins of waterfall (FT4 needs less: 156 x 2 x 2 x 131 = 81,744) and nfft
// 1920. One set per channel, static, in PSRAM (docs/architecture/memory.md): the CPU copies into the
// ring, so no DMA reaches it. A channel that needs more is refused at start, never moved to the heap.
// Only the P4 links this component.
static constexpr int kChannelNfftMax = 1920;
static constexpr size_t kChannelWaterfallBytes = 161076;
struct ChannelBuffers {
    WF_ELEM_T mag[kChannelWaterfallBytes / sizeof(WF_ELEM_T)];
    alignas(16) uint8_t fft_work[MONITOR_FFT_WORK_BYTES(kChannelNfftMax)];
    float window[kChannelNfftMax];
    float last_frame[kChannelNfftMax];
    kiss_fft_scalar timedata[kChannelNfftMax];
    kiss_fft_cpx freqdata[kChannelNfftMax / 2 + 1];
    float ring[kIngestRingSamples];
};
static DECODE_PSRAM_BSS ChannelBuffers s_channel_buffers[RX_MAX_CHANNELS];

// One channel's capture state (docs/roadmap/several-channels.md, A channel): its monitor and waterfall,
// the AGC, its ring, and where it is in its slot. Lives on the capture task's stack for one run.
struct Ft8Channel {
    const ft8_audio_pipeline_config_t* cfg;
    const char* tag;
    monitor_config_t mon_cfg;
    monitor_t mon;
    float* block;       // the block being filled, heap
    int block_fill;
    float* block_gain;  // the AGC gain applied to each waterfall block: the decode SNR divides it back out (decode.md, SNR)
    Ft8SampleRing ring;
    int64_t slot_ms;
    int target_blocks;
    int block_ms;
    int64_t slot_idx;
    int slot_blocks;
    int64_t slot_samples;
    unsigned dropped_at_boundary;
    // monitor_process() time this slot, for BDRY's mon: the capture path's cost per channel
    // (docs/roadmap/several-channels.md, A channel), timed in place, where the cache is not warm as in decode_bench.
    int64_t slot_monitor_us;
    // The first slot has no boundary behind it to vouch for its start.
    bool slot_clean;
    ft8_activity_batch_t activity;
    ft8_activity_batch_t activity_out;
    // The waterfall's span: bins are tone-spacing wide, from min_bin up to max_bin.
    int activity_lo_hz;
    int activity_hi_hz;
};

static void channel_free(Ft8Channel* ch)
{
    free(ch->block);
    free(ch->block_gain);
    ch->block = nullptr;
    ch->block_gain = nullptr;
    monitor_free(&ch->mon);
}

// Sets up the channel in its own buffers. False, logged, when they cannot hold its configuration or
// the heap has no room for its block buffers.
static bool channel_start(Ft8Channel* ch, const ft8_audio_pipeline_config_t* cfg, const char* tag)
{
    memset(ch, 0, sizeof(*ch));
    ch->cfg = cfg;
    ch->tag = tag;
    if (cfg->channel < 0 || cfg->channel >= RX_MAX_CHANNELS || cfg->mode == nullptr) {
        ESP_LOGE(tag, "channel %d refused: %d channels, mode %s", cfg->channel, RX_MAX_CHANNELS,
                 cfg->mode ? cfg->mode->name : "none");
        return false;
    }
    ChannelBuffers* buffers = &s_channel_buffers[cfg->channel];
    ch->mon_cfg = {
        .f_min = 200.0f,
        .f_max = 2900.0f,
        .sample_rate = FT8_SAMPLE_RATE,
        .time_osr = cfg->time_osr,
        .freq_osr = cfg->freq_osr,
        .protocol = cfg->mode->protocol_id,
    };
    const monitor_buffers_t monitor_buffers = {
        .mag = buffers->mag,
        .mag_bytes = sizeof(buffers->mag),
        .fft_work = buffers->fft_work,
        .fft_work_bytes = sizeof(buffers->fft_work),
        .window = buffers->window,
        .last_frame = buffers->last_frame,
        .timedata = buffers->timedata,
        .freqdata = buffers->freqdata,
        .nfft_max = kChannelNfftMax,
    };

    log_heap("AUDIO_PIPE_BEFORE_MONITOR_INIT");
    monitor_init_buffers(&ch->mon, &ch->mon_cfg, &monitor_buffers);
    log_heap("AUDIO_PIPE_AFTER_MONITOR_INIT");
    if (ch->mon.wf.mag == nullptr) {
        ESP_LOGE(tag, "monitor init failed: channel %d %s t%d/f%d does not fit its buffers", cfg->channel,
                 cfg->mode->name, cfg->time_osr, cfg->freq_osr);
        return false;
    }
    ESP_LOGI(tag, "channel %d %s monitor nfft=%d freq_osr=%d waterfall=%d bytes", cfg->channel, cfg->mode->name,
             ch->mon.nfft, ch->mon.wf.freq_osr, ch->mon.wf.max_blocks * ch->mon.wf.block_stride);
    monitor_reset(&ch->mon);

    ch->block = (float*)heap_caps_malloc(sizeof(float) * ch->mon.block_size, MALLOC_CAP_DEFAULT);
    ch->block_gain = (float*)heap_caps_malloc(sizeof(float) * ch->mon.wf.max_blocks, MALLOC_CAP_DEFAULT);
    if (!ch->block || !ch->block_gain) {
        ESP_LOGE(tag, "pipeline buffer allocation failed");
        channel_free(ch);
        return false;
    }

    ft8_sample_ring_init(&ch->ring, buffers->ring, kIngestRingSamples);
    ch->slot_ms = cfg->mode->slot_time_ms;
    ch->target_blocks = cfg->mode->total_symbols + 1;
    ch->block_ms = (int)lroundf(cfg->mode->symbol_period * 1000.0f);
    ft8_activity_batch_init(&ch->activity, ch->block_ms);
    ch->activity_lo_hz = (int)lroundf(ch->mon.min_bin / ch->mon.symbol_period);
    ch->activity_hi_hz = (int)lroundf(ch->mon.max_bin / ch->mon.symbol_period);
    return true;
}

static void send_activity(const Ft8Channel* ch, const ft8_activity_batch_t& b)
{
    ch->cfg->on_activity(ch->cfg->ctx, b.slot_idx, ch->block_ms, b.first, b.level, &b.mask[0][0], b.n,
                         ch->activity_lo_hz, ch->activity_hi_hz);
}

// A full block is in ch->block: AGC, waterfall, activity, then the slot boundary or the decode trigger.
// `queued` is the samples behind this block's last one, which dates the block by its audio.
static void channel_block(Ft8Channel* ch, int64_t queued)
{
    const ft8_audio_pipeline_config_t* cfg = ch->cfg;
    monitor_t& mon = ch->mon;
    float* block = ch->block;

    double acc = 0.0;
    for (int j = 0; j < mon.block_size; ++j) {
        acc += fabsf(block[j]);
    }
    float level = (float)(acc / mon.block_size);
    float gain = (level > 1e-6f) ? 0.1f / level : 1.0f;
    if (gain < 0.1f) gain = 0.1f;
    if (gain > 10.0f) gain = 10.0f;
    for (int j = 0; j < mon.block_size; ++j) {
        block[j] *= gain;
    }

    // Band activity from the column this block adds: its first time and frequency
    // subdivision, the 6.25 Hz grid. Not while we transmit: that audio is not the band.
    int activity_level = -1;
    uint8_t activity_mask[FT8_ACTIVITY_MASK_BYTES];
    if (mon.wf.num_blocks < ch->target_blocks && mon.wf.num_blocks < mon.wf.max_blocks) {
        ch->block_gain[mon.wf.num_blocks] = gain;
        const int64_t monitor_t0 = esp_timer_get_time();
        monitor_process(&mon, block);
        ch->slot_monitor_us += esp_timer_get_time() - monitor_t0;
        if (cfg->on_activity && !decode_tx_state_get_tx_active() &&
            !decode_tx_state_get_was_txing()) {
            activity_level = ft8_block_activity(
                mon.wf.mag + (mon.wf.num_blocks - 1) * mon.wf.block_stride,
                mon.wf.num_bins, activity_mask);
        }
    }

    if (cfg->on_block_processed) {
        cfg->on_block_processed(cfg->ctx);
    }

    // No pacing: a block is processed as soon as its last sample is in
    // the ring, so decode starts within a tick of its 79th block's audio.
    // Pacing to a 160 ms grid held each block up to a grid step, and
    // with blocks dated by their audio that lag came off the guard.
    ch->block_fill = 0;

    ch->slot_blocks++;
    // Date the block by its audio, not by when it is processed.
    const int64_t block_now_ms = rtc_now_ms();
    int64_t now_idx = ft8_block_slot_idx(block_now_ms, queued, (int)ch->slot_ms, FT8_SAMPLE_RATE);
    if (activity_level >= 0) {
        const int pos = ft8_block_pos(block_now_ms, queued, (int)ch->slot_ms, ch->block_ms, FT8_SAMPLE_RATE);
        if (ft8_activity_batch_add(&ch->activity, now_idx, pos, (uint8_t)activity_level,
                                   activity_mask, &ch->activity_out)) {
            send_activity(ch, ch->activity_out);
        }
    }
    if (now_idx != ch->slot_idx) {
        const int64_t lost = ft8_slot_sample_deficit(
            ch->slot_samples, now_idx - ch->slot_idx, (int)ch->slot_ms, FT8_SAMPLE_RATE);
        const unsigned dropped = ft8_sample_ring_dropped(&ch->ring);
        const unsigned drop = dropped - ch->dropped_at_boundary;
        ch->dropped_at_boundary = dropped;
        // The next slot starts on its audio only if one boundary was crossed,
        // the slot just ended was whole, and the ring kept every sample. A
        // clock set or step cuts the slot short; a drop misdates the queue.
        ch->slot_clean = now_idx == ch->slot_idx + 1 && lost <= mon.block_size && drop == 0;
        // g is blocks captured after the decode reset: about 14 is a full guard.
        // Port A log, not the console UART: the sidekick prints it.
        {
            char buf[64];
            // fe: the receive front end's time since the last boundary (ingest task).
            const unsigned frontend_us =
                cfg->take_frontend_us ? (unsigned)cfg->take_frontend_us(cfg->ctx) : 0;
            snprintf(buf, sizeof(buf), "BDRY %lld g%d lost%lld drop%u mon%lld fe%u",
                     (long long)ch->slot_idx, ch->slot_blocks, (long long)lost, drop,
                     (long long)ch->slot_monitor_us, frontend_us);
            debug_log_line(buf);
        }
        if (ch->slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
            decode_tx_state_set_decode_applied_slot_idx(ch->slot_idx);
        }
        if (ch->slot_idx == decode_tx_state_get_tx_slot_idx()) {
            decode_tx_state_set_tx_slot_idx(-1);
        }
        ch->slot_idx = now_idx;
        ch->slot_blocks = 0;
        ch->slot_samples = 0;
        ch->slot_monitor_us = 0;
        mon.wf.num_blocks = 0;
        monitor_reset(&mon);
    } else if (ch->slot_blocks >= cfg->mode->total_symbols &&
               mon.wf.num_blocks >= cfg->mode->total_symbols) {
        ESP_LOGD(ch->tag, "Triggering decode at slot %lld blocks=%d wf=%d",
                 (long long)ch->slot_idx, ch->slot_blocks, mon.wf.num_blocks);
        // The decode holds this task for about 2 s: send the slot's last blocks first.
        if (cfg->on_activity && ft8_activity_batch_take(&ch->activity, &ch->activity_out)) {
            send_activity(ch, ch->activity_out);
        }
        // tx_slot_idx outlives was_txing. The slot loop clears that
        // flag ~10 ms after the tones end, and the tones end at the
        // 79th block, so the flag is often already false here.
        if (decode_tx_state_get_tx_active() || decode_tx_state_get_was_txing() ||
            ch->slot_idx == decode_tx_state_get_tx_slot_idx()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "SKIP %lld", (long long)ch->slot_idx);
            debug_log_line(buf);
            if (ch->slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                decode_tx_state_set_decode_applied_slot_idx(ch->slot_idx);
            }
            if (ch->slot_idx == decode_tx_state_get_tx_slot_idx()) {
                decode_tx_state_set_tx_slot_idx(-1);
            }
        } else if (decode_tx_state_get_decode_enabled()) {
            const ft8_slot_t slot = {
                .channel = cfg->channel,
                .mode = cfg->mode,
                .monitor = &mon,
                .block_gain = ch->block_gain,
                .slot_idx = ch->slot_idx,
                .capture_clean =
                    ch->slot_clean && ft8_sample_ring_dropped(&ch->ring) == ch->dropped_at_boundary,
            };
            decode_tx_state_set_decode_in_progress(true);
            decode_slot(&slot);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "PAUSED %lld", (long long)ch->slot_idx);
            debug_log_line(buf);
            if (ch->slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
                decode_tx_state_set_decode_applied_slot_idx(ch->slot_idx);
            }
        }
        monitor_reset(&mon);
        mon.wf.num_blocks = 0;
        ch->slot_blocks = 0;
    }
}

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

bool ft8_audio_pipeline_run(const ft8_audio_pipeline_config_t* cfg)
{
    if (!cfg || !cfg->read || !cfg->should_stop) return false;
    const char* tag = cfg->tag ? cfg->tag : "FT8_AUDIO";

    // On this task's stack: pointers and counters only, under 1 KB; the buffers are the channel's statics.
    Ft8Channel ch;
    if (!channel_start(&ch, cfg, tag)) return false;

    float* temp_dec = (float*)heap_caps_malloc(sizeof(float) * 512, MALLOC_CAP_DEFAULT);
    log_heap("AUDIO_PIPE_AFTER_SAMPLE_BUFFERS");
    if (!temp_dec) {
        ESP_LOGE(tag, "pipeline buffer allocation failed");
        channel_free(&ch);
        return false;
    }

    int64_t now_ms = rtc_now_ms();
    int64_t rem = now_ms % ch.slot_ms;
    int64_t wait_ms = (rem < 100) ? 0 : (ch.slot_ms - rem);
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
    }

    AudioIngest ingest = { cfg, &ch.ring, false, false };
    // Higher priority than this task. A USB read must never wait out a decode (about 2 s of the
    // core that runs it), so ingest is off the decode task's core: core 0, where the main loop and
    // the USB tasks run, while decode holds core 1 (main.cpp pins decode_audio there). Pinned, not
    // merely unpinned: ESP-IDF pins a task to the core it first uses the FPU on (docs, FreeRTOS
    // IDF, Floating Point Usage; vPortTaskPinToCore() in portable/riscv/port.c), and ingest
    // runs float code from its first read, so unpinned it would be pinned to whichever core
    // that first read fell on. docs/architecture/main_board.md, Tasks.
    if (xTaskCreatePinnedToCore(audio_ingest_task, "audio_ingest", 6144, &ingest, 5,
                                nullptr, 0) != pdPASS) {
        ESP_LOGE(tag, "audio ingest task create failed");
        free(temp_dec);
        channel_free(&ch);
        return false;
    }
    ESP_LOGI(tag, "audio ingest ring=%u samples in %s, fft frame in %s", kIngestRingSamples,
             esp_ptr_external_ram(ch.ring.samples) ? "psram" : "internal",
             esp_ptr_external_ram(ch.mon.last_frame) ? "psram" : "internal");

    ch.slot_idx = rtc_now_ms() / ch.slot_ms;

    while (!cfg->should_stop(cfg->ctx)) {
        int samples_dec = (int)ft8_sample_ring_read(&ch.ring, temp_dec, 512);
        if (samples_dec <= 0) {
            // One tick, 10 ms at the P4's 100 Hz. pdMS_TO_TICKS(1) is 0 there: a
            // yield, which would spin this task and starve core 1's idle task.
            vTaskDelay(1);
            continue;
        }
        if (samples_dec > 512) samples_dec = 512;

        for (int i = 0; i < samples_dec && !cfg->should_stop(cfg->ctx); i++) {
            ch.block[ch.block_fill++] = temp_dec[i];
            ch.slot_samples++;
            if (ch.block_fill >= ch.mon.block_size) {
                channel_block(&ch, (int64_t)ft8_sample_ring_count(&ch.ring) + (samples_dec - i - 1));
            }
        }
    }

    ingest.stop = true;
    for (int i = 0; i < 50 && !ingest.done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(temp_dec);
    channel_free(&ch);
    return true;
}
