#include "ft8_audio_pipeline.h"
#include "ft8_sample_ring.h"
#include "ft8_slot_deficit.h"
#include "ft8_block_activity.h"
#include "decode_tx_state.h"
#include "debug_log.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"

extern "C" {
#include "common/decode_psram.h"
#include "common/monitor.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
}

// Defined by decode_pipeline (decode_pipeline.h, decode_budget.h), which this component does not depend on.
void decode_slot(const ft8_slot_t* slot);
extern "C" int decode_trigger_blocks(int ft4, int64_t period_idx, int beside_other);
int64_t rtc_now_ms();

#ifndef FT8_SAMPLE_RATE
#define FT8_SAMPLE_RATE 6000
#endif

// 2.7 s at 6 kHz. Sized when decode held the capture task for about 2 s (the 20 m run dropped about
// 2 s of audio); decode now runs on its own task, so the ring covers USB jitter and a busy core only.
static constexpr unsigned kIngestRingSamples = 16384;

// One channel's capture buffers, sized for the largest configuration a channel runs: FT8 at t2/f2,
// 93 blocks x 2 x 2 x 433 bins of waterfall (FT4 needs less: 156 x 2 x 2 x 131 = 81,744) and nfft
// 1920. Two waterfalls in turn: capture writes one while decode reads and subtracts from the other
// (docs/architecture/decode.md, Channels). One set per channel, static, in PSRAM (docs/architecture/memory.md): the CPU copies into the
// ring, so no DMA reaches it. A channel that needs more is refused at start, never moved to the heap.
// Only the P4 links this component.
static constexpr int kChannelNfftMax = 1920;
static constexpr size_t kChannelWaterfallBytes = 161076;
static constexpr int kWaterfalls = 2;
struct ChannelBuffers {
    WF_ELEM_T mag[kWaterfalls][kChannelWaterfallBytes / sizeof(WF_ELEM_T)];
    alignas(16) uint8_t fft_work[MONITOR_FFT_WORK_BYTES(kChannelNfftMax)];
    float window[kChannelNfftMax];
    float last_frame[kChannelNfftMax];
    kiss_fft_scalar timedata[kChannelNfftMax];
    kiss_fft_cpx freqdata[kChannelNfftMax / 2 + 1];
    float ring[kIngestRingSamples];
};
static DECODE_PSRAM_BSS ChannelBuffers s_channel_buffers[RX_MAX_CHANNELS];

// Diagnostics for a slot whose audio came up short with nothing dropped (`lost` without `drop`, 2026-11-07):
// the longest wait between reads that returned samples, when it ended, and the reads that returned none.
// The ingest task writes them, the station's channel takes them at its boundary and logs a STALL line when
// the wait passed kStallLogUs. A stalled ingest task shows a long wait with no empty reads; a source that
// sent nothing shows the empty reads, each up to the read's own timeout.
static std::atomic<uint32_t> s_ingest_gap_max_us{0};
static std::atomic<int64_t> s_ingest_gap_end_ms{0};
static std::atomic<uint32_t> s_ingest_empty{0};
static constexpr uint32_t kStallLogUs = 50000;


// One channel's capture state (docs/roadmap/several-channels.md, A channel): its monitor and waterfall,
// the AGC, its ring, and where it is in its slot. Lives on the capture task's stack for one run;
// ft8_audio_pipeline_run() does not return while the decode task still reads any of it.
struct Ft8Channel {
    const ft8_audio_pipeline_config_t* cfg;    // the source's callbacks
    const ft8_audio_channel_config_t* chan;    // this channel
    bool beside_other;                         // the source carries another channel too
    const char* tag;
    // Port A log names: "SLOT", "BDRY", ... for the station's channel, which tools/ parse as they always
    // have; "SLOT/FT4", "BDRY/FT4", ... for a channel that only listens.
    char suffix[8];
    monitor_config_t mon_cfg;
    monitor_t mon;      // capture's: its wf.mag is the waterfall being filled, mag[filling]
    float* block;       // the block being filled, heap
    int block_fill;
    // The AGC gain applied to each waterfall block, one array per waterfall: the decode SNR divides it
    // back out (decode.md, SNR). Heap.
    float* block_gain[kWaterfalls];
    int filling;        // the waterfall capture writes
    // Per waterfall: the monitor as it stood at the decode trigger, which the decode task reads, and
    // whether the decode task still has it. Capture sets `decoding` before the hand-off; the decode task
    // clears it, with release, once decode_slot() has returned.
    monitor_t decode_mon[kWaterfalls];
    std::atomic<bool> decoding[kWaterfalls];
    int64_t handed_slot_idx;  // the last slot handed to decode, which marks it applied itself
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
    // The slot began at a boundary, not where this run started: a run's first slot is cut short by the
    // restart and its read gap is the source coming up, so it sends no STALL.
    bool slot_from_boundary;
    ft8_activity_batch_t activity;
    ft8_activity_batch_t activity_out;
    // The waterfall's span: bins are tone-spacing wide, from min_bin up to max_bin.
    int activity_lo_hz;
    int activity_hi_hz;
};

static void channel_free(Ft8Channel* ch)
{
    free(ch->block);
    ch->block = nullptr;
    for (int w = 0; w < kWaterfalls; ++w) {
        free(ch->block_gain[w]);
        ch->block_gain[w] = nullptr;
    }
    monitor_free(&ch->mon);
}

// ---- The decode task: decodes each finished slot off the capture task (docs/architecture/main_board.md,
// Tasks). One for every channel, created on first use and kept, so a radio's restart does not churn it. ----
// A finished slot, and the flag the decode task clears when it is done with that slot's waterfall.
struct DecodeJob {
    ft8_slot_t slot;
    std::atomic<bool>* done;
};
static QueueHandle_t s_decode_queue;

static void decode_task(void*)
{
    for (;;) {
        DecodeJob job;
        if (xQueueReceive(s_decode_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        decode_slot(&job.slot);
        job.done->store(false, std::memory_order_release);
    }
}

// Below the capture task's priority on the same core (main.cpp pins that to core 1 at 4), so a block
// is processed as it arrives while a decode runs; the stack decode_slot() had on the capture task.
static constexpr UBaseType_t kDecodePriority = 3;
static constexpr uint32_t kDecodeStack = 8192;

static bool decode_task_start(const char* tag)
{
    if (s_decode_queue) return true;
    static StaticQueue_t queue_buffer;
    static uint8_t queue_storage[kWaterfalls * RX_MAX_CHANNELS * sizeof(DecodeJob)];
    s_decode_queue = xQueueCreateStatic(kWaterfalls * RX_MAX_CHANNELS, sizeof(DecodeJob), queue_storage, &queue_buffer);
    if (xTaskCreatePinnedToCore(decode_task, "ft8_decode", kDecodeStack, nullptr, kDecodePriority, nullptr, 1) != pdPASS) {
        ESP_LOGE(tag, "decode task create failed");
        vQueueDelete(s_decode_queue);
        s_decode_queue = nullptr;
        return false;
    }
    return true;
}

// Sets up the channel in its own buffers. False, logged, when they cannot hold its configuration or
// the heap has no room for its block buffers.
static bool channel_start(Ft8Channel* ch, const ft8_audio_pipeline_config_t* cfg,
                          const ft8_audio_channel_config_t* chan, const char* tag)
{
    new (ch) Ft8Channel();  // every field zero, the atomics false
    ch->handed_slot_idx = -1;
    ch->cfg = cfg;
    ch->chan = chan;
    ch->beside_other = cfg->n_channels > 1;
    ch->tag = tag;
    if (chan->channel < 0 || chan->channel >= RX_MAX_CHANNELS || chan->mode == nullptr) {
        ESP_LOGE(tag, "channel %d refused: %d channels, mode %s", chan->channel, RX_MAX_CHANNELS,
                 chan->mode ? chan->mode->name : "none");
        return false;
    }
    if (chan->station) {
        ch->suffix[0] = '\0';
    } else {
        snprintf(ch->suffix, sizeof(ch->suffix), "/%s", chan->mode->name);
    }
    ChannelBuffers* buffers = &s_channel_buffers[chan->channel];
    ch->mon_cfg = {
        .f_min = 200.0f,
        .f_max = 2900.0f,
        .sample_rate = FT8_SAMPLE_RATE,
        .time_osr = chan->time_osr,
        .freq_osr = chan->freq_osr,
        .protocol = chan->mode->protocol_id,
    };
    const monitor_buffers_t monitor_buffers = {
        .mag = buffers->mag[0],
        .mag_bytes = sizeof(buffers->mag[0]),
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
        ESP_LOGE(tag, "monitor init failed: channel %d %s t%d/f%d does not fit its buffers", chan->channel,
                 chan->mode->name, chan->time_osr, chan->freq_osr);
        return false;
    }
    ESP_LOGI(tag, "channel %d %s monitor nfft=%d freq_osr=%d waterfall=%d bytes", chan->channel, chan->mode->name,
             ch->mon.nfft, ch->mon.wf.freq_osr, ch->mon.wf.max_blocks * ch->mon.wf.block_stride);
    monitor_reset(&ch->mon);

    ch->block = (float*)heap_caps_malloc(sizeof(float) * ch->mon.block_size, MALLOC_CAP_DEFAULT);
    bool gains = true;
    for (int w = 0; w < kWaterfalls; ++w) {
        ch->block_gain[w] = (float*)heap_caps_malloc(sizeof(float) * ch->mon.wf.max_blocks, MALLOC_CAP_DEFAULT);
        gains = gains && ch->block_gain[w];
    }
    ch->filling = 0;
    if (!ch->block || !gains) {
        ESP_LOGE(tag, "pipeline buffer allocation failed");
        channel_free(ch);
        return false;
    }

    ft8_sample_ring_init(&ch->ring, buffers->ring, kIngestRingSamples);
    ch->slot_ms = chan->mode->slot_time_ms;
    // Filled up to the latest start this mode can have (decode_trigger_blocks()), and one block more.
    const int ft4 = chan->mode->protocol_id == FTX_PROTOCOL_FT4;
    const int t0 = decode_trigger_blocks(ft4, 0, ch->beside_other);
    const int t1 = decode_trigger_blocks(ft4, 1, ch->beside_other);
    ch->target_blocks = (t0 > t1 ? t0 : t1) + 1;
    ch->block_ms = (int)lroundf(chan->mode->symbol_period * 1000.0f);
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
// Marks a slot of the station's channel applied for the TX gate (decode_tx_state.h); another channel's
// slots are not the station's and touch nothing.
static void mark_applied(const Ft8Channel* ch, int64_t slot_idx)
{
    if (ch->chan->station && slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
        decode_tx_state_set_decode_applied_slot_idx(slot_idx);
    }
}

// Blocks this slot's decode starts after: its mode's, and for FT4's second period beside FT8 an earlier one
// (decode_limits.h; docs/architecture/decode.md, Decoding later in the slot).
static int trigger_blocks(const Ft8Channel* ch)
{
    return decode_trigger_blocks(ch->chan->mode->protocol_id == FTX_PROTOCOL_FT4, ch->slot_idx, ch->beside_other);
}

static void log_slot_event(const Ft8Channel* ch, const char* what, int64_t slot_idx)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "%s%s %lld", what, ch->suffix, (long long)slot_idx);
    debug_log_line(buf);
}

static void channel_block(Ft8Channel* ch, int64_t queued)
{
    const ft8_audio_pipeline_config_t* cfg = ch->cfg;
    const ft8_audio_channel_config_t* chan = ch->chan;
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
        ch->block_gain[ch->filling][mon.wf.num_blocks] = gain;
        const int64_t monitor_t0 = esp_timer_get_time();
        monitor_process(&mon, block);
        ch->slot_monitor_us += esp_timer_get_time() - monitor_t0;
        // The slot bar shows the station's channel.
        if (cfg->on_activity && chan->station && !decode_tx_state_get_tx_active() &&
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
                (chan->station && cfg->take_frontend_us) ? (unsigned)cfg->take_frontend_us(cfg->ctx) : 0;
            // fe is the source's, once: on the station's channel.
            snprintf(buf, sizeof(buf), "BDRY%s %lld g%d lost%lld drop%u mon%lld fe%u", ch->suffix,
                     (long long)ch->slot_idx, ch->slot_blocks, (long long)lost, drop,
                     (long long)ch->slot_monitor_us, chan->station ? frontend_us : 0u);
            debug_log_line(buf);
        }
        if (chan->station) {
            const uint32_t gap_us = s_ingest_gap_max_us.exchange(0, std::memory_order_relaxed);
            const int64_t gap_end_ms = s_ingest_gap_end_ms.load(std::memory_order_relaxed);
            const uint32_t empty = s_ingest_empty.exchange(0, std::memory_order_relaxed);
            if (gap_us > kStallLogUs && ch->slot_from_boundary) {
                char buf[64];
                snprintf(buf, sizeof(buf), "STALL %lld gap%u empty%u at%lld", (long long)ch->slot_idx,
                         (unsigned)(gap_us / 1000), (unsigned)empty, (long long)(gap_end_ms % ch->slot_ms));
                debug_log_line(buf);
            }
        }
        // A slot handed to decode is marked applied by decode_slot(), after it publishes; one never
        // decoded (cut short, or dropped by a clock step) is marked here.
        if (ch->slot_idx != ch->handed_slot_idx) mark_applied(ch, ch->slot_idx);
        if (chan->station && ch->slot_idx == decode_tx_state_get_tx_slot_idx()) {
            decode_tx_state_set_tx_slot_idx(-1);
        }
        ch->slot_idx = now_idx;
        ch->slot_from_boundary = true;
        ch->slot_blocks = 0;
        ch->slot_samples = 0;
        ch->slot_monitor_us = 0;
        mon.wf.num_blocks = 0;
        monitor_reset(&mon);
    } else if (ch->slot_blocks >= trigger_blocks(ch) && mon.wf.num_blocks >= trigger_blocks(ch)) {
        ESP_LOGD(ch->tag, "Triggering decode at slot %lld blocks=%d wf=%d",
                 (long long)ch->slot_idx, ch->slot_blocks, mon.wf.num_blocks);
        // The slot's last blocks close its activity at the trigger, as its decodes follow.
        if (cfg->on_activity && chan->station && ft8_activity_batch_take(&ch->activity, &ch->activity_out)) {
            send_activity(ch, ch->activity_out);
        }
        // tx_slot_idx outlives was_txing. The slot loop clears that
        // flag ~10 ms after the tones end, and the tones end at the
        // 79th block, so the flag is often already false here. tx_slot_idx
        // counts the station's slots, so another channel goes by the flags: an
        // FT8 transmission keys from 0.5 to 13.14 s, over both FT4 periods'
        // triggers (5.04 and 12.54 s), so each sees tx_active.
        if (decode_tx_state_get_tx_active() || decode_tx_state_get_was_txing() ||
            (chan->station && ch->slot_idx == decode_tx_state_get_tx_slot_idx())) {
            log_slot_event(ch, "SKIP", ch->slot_idx);
            mark_applied(ch, ch->slot_idx);
            if (chan->station && ch->slot_idx == decode_tx_state_get_tx_slot_idx()) {
                decode_tx_state_set_tx_slot_idx(-1);
            }
        } else if (decode_tx_state_get_decode_enabled() &&
                   ch->decoding[1 - ch->filling].load(std::memory_order_acquire)) {
            // The decode task still has the other waterfall, from a slot ago: a decode a whole slot
            // long. Capture cannot hand this one over without a waterfall to fill, so it is not decoded.
            log_slot_event(ch, "BUSY", ch->slot_idx);
            mark_applied(ch, ch->slot_idx);
        } else if (decode_tx_state_get_decode_enabled()) {
            // Hand this waterfall to the decode task, and fill the other from here on.
            const int w = ch->filling;
            ch->decode_mon[w] = mon;
            const DecodeJob job = {
                .slot = {
                    .channel = chan->channel,
                    .mode = chan->mode,
                    .station = chan->station,
                    .dial_offset_hz = chan->dial_offset_hz,
                    .beside_other = ch->beside_other,
                    .monitor = &ch->decode_mon[w],
                    .block_gain = ch->block_gain[w],
                    .slot_idx = ch->slot_idx,
                    .capture_clean =
                        ch->slot_clean && ft8_sample_ring_dropped(&ch->ring) == ch->dropped_at_boundary,
                },
                .done = &ch->decoding[w],
            };
            ch->decoding[w].store(true, std::memory_order_relaxed);
            if (chan->station) decode_tx_state_set_decode_in_progress(true);
            ch->handed_slot_idx = ch->slot_idx;
            // The queue holds every waterfall of every channel, so this never waits.
            xQueueSend(s_decode_queue, &job, portMAX_DELAY);
            ch->filling = 1 - w;
            mon.wf.mag = s_channel_buffers[chan->channel].mag[ch->filling];
        } else {
            log_slot_event(ch, "PAUSED", ch->slot_idx);
            mark_applied(ch, ch->slot_idx);
        }
        monitor_reset(&mon);
        mon.wf.num_blocks = 0;
        ch->slot_blocks = 0;
    }
}


// One read's samples per channel. A QMX read is 1 ms of USB audio, 6 samples at 6 kHz; this is room for a
// burst. Static: one ingest task runs at a time.
static constexpr int kIngestBlock = 512;
static DECODE_PSRAM_BSS float s_ingest_tmp[RX_MAX_CHANNELS][kIngestBlock];

struct AudioIngest {
    const ft8_audio_pipeline_config_t *cfg;
    Ft8SampleRing *rings[RX_MAX_CHANNELS];
    volatile bool stop;
    volatile bool done;
};

static void audio_ingest_task(void *arg)
{
    AudioIngest *in = static_cast<AudioIngest *>(arg);
    float* outs[RX_MAX_CHANNELS];
    for (int c = 0; c < RX_MAX_CHANNELS; ++c) outs[c] = s_ingest_tmp[c];
    const int n_channels = in->cfg->n_channels;
    int64_t last_ok_us = 0;
    while (!in->stop && !in->cfg->should_stop(in->cfg->ctx)) {
        int n = in->cfg->read(in->cfg->ctx, outs, n_channels, kIngestBlock);
        if (n <= 0) {
            s_ingest_empty.fetch_add(1, std::memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        const int64_t now_us = esp_timer_get_time();
        if (last_ok_us != 0) {
            const uint32_t gap = (uint32_t)(now_us - last_ok_us);
            if (gap > s_ingest_gap_max_us.load(std::memory_order_relaxed)) {
                s_ingest_gap_max_us.store(gap, std::memory_order_relaxed);
                s_ingest_gap_end_ms.store(rtc_now_ms(), std::memory_order_relaxed);
            }
        }
        last_ok_us = now_us;
        if (n > kIngestBlock) n = kIngestBlock;
        for (int c = 0; c < n_channels; ++c) ft8_sample_ring_write(in->rings[c], outs[c], (unsigned)n);
    }
    in->done = true;
    vTaskDelete(nullptr);
}

// Feeds one channel what its ring holds, up to one read. True if there was anything.
static bool channel_drain(Ft8Channel* ch, float* temp, bool (*stop)(void*), void* ctx)
{
    int n = (int)ft8_sample_ring_read(&ch->ring, temp, kIngestBlock);
    if (n <= 0) return false;
    for (int i = 0; i < n && !stop(ctx); i++) {
        ch->block[ch->block_fill++] = temp[i];
        ch->slot_samples++;
        if (ch->block_fill >= ch->mon.block_size) {
            channel_block(ch, (int64_t)ft8_sample_ring_count(&ch->ring) + (n - i - 1));
        }
    }
    return true;
}

bool ft8_audio_pipeline_run(const ft8_audio_pipeline_config_t* cfg)
{
    if (!cfg || !cfg->read || !cfg->should_stop) return false;
    const char* tag = cfg->tag ? cfg->tag : "FT8_AUDIO";
    const int n_channels = cfg->n_channels;
    if (n_channels < 1 || n_channels > RX_MAX_CHANNELS) {
        ESP_LOGE(tag, "%d channels refused: the image holds %d", n_channels, RX_MAX_CHANNELS);
        return false;
    }

    if (!decode_task_start(tag)) return false;
    // On this task's stack: pointers, counters and two monitor copies a channel, about 1 KB each; the
    // buffers are each channel's statics.
    Ft8Channel ch[RX_MAX_CHANNELS];
    int started = 0;
    int station = 0;
    for (; started < n_channels; ++started) {
        if (!channel_start(&ch[started], cfg, &cfg->channels[started], tag)) break;
        if (cfg->channels[started].station) station = started;
    }
    if (started < n_channels) {
        for (int c = 0; c < started; ++c) channel_free(&ch[c]);
        return false;
    }

    float* temp_dec = (float*)heap_caps_malloc(sizeof(float) * kIngestBlock, MALLOC_CAP_DEFAULT);
    log_heap("AUDIO_PIPE_AFTER_SAMPLE_BUFFERS");
    if (!temp_dec) {
        ESP_LOGE(tag, "pipeline buffer allocation failed");
        for (int c = 0; c < n_channels; ++c) channel_free(&ch[c]);
        return false;
    }

    // Start on the station channel's boundary, which is every other channel's too: an FT4 period is half
    // an FT8 slot.
    const int64_t slot_ms = ch[station].slot_ms;
    int64_t now_ms = rtc_now_ms();
    int64_t rem = now_ms % slot_ms;
    int64_t wait_ms = (rem < 100) ? 0 : (slot_ms - rem);
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
    }

    AudioIngest ingest = {};
    ingest.cfg = cfg;
    for (int c = 0; c < n_channels; ++c) ingest.rings[c] = &ch[c].ring;
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
        for (int c = 0; c < n_channels; ++c) channel_free(&ch[c]);
        return false;
    }
    ESP_LOGI(tag, "audio ingest %d channel(s), ring=%u samples in %s, fft frame in %s", n_channels,
             kIngestRingSamples, esp_ptr_external_ram(ch[0].ring.samples) ? "psram" : "internal",
             esp_ptr_external_ram(ch[0].mon.last_frame) ? "psram" : "internal");

    for (int c = 0; c < n_channels; ++c) ch[c].slot_idx = rtc_now_ms() / ch[c].slot_ms;

    while (!cfg->should_stop(cfg->ctx)) {
        bool any = false;
        for (int c = 0; c < n_channels; ++c) {
            any = channel_drain(&ch[c], temp_dec, cfg->should_stop, cfg->ctx) || any;
        }
        if (!any) {
            // One tick, 10 ms at the P4's 100 Hz. pdMS_TO_TICKS(1) is 0 there: a
            // yield, which would spin this task and starve core 1's idle task.
            vTaskDelay(1);
        }
    }

    ingest.stop = true;
    for (int i = 0; i < 50 && !ingest.done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // The decode task reads each channel's waterfalls, gains and monitor copies until it is done
    // with them; the next run would overwrite them. A decode is about 2 s.
    for (int c = 0; c < n_channels; ++c) {
        for (int w = 0; w < kWaterfalls; ++w) {
            while (ch[c].decoding[w].load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    free(temp_dec);
    for (int c = 0; c < n_channels; ++c) channel_free(&ch[c]);
    return true;
}
