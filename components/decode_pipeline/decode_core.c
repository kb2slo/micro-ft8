#include "decode_core.h"

#include <string.h>

#include "decode_subtract.h"

int decode_core_begin(decode_core_t* core, monitor_t* monitor, const float* block_gain,
                      const decode_core_config_t* config)
{
    core->config = config;
    core->monitor = monitor;
    core->n_tones = (monitor->wf.protocol == FTX_PROTOCOL_FT4) ? 4 : 8;
    core->subtract = config->passes > 0;
    int passes = config->passes;
    if (passes < 1) passes = 1;
    if (passes > DECODE_CORE_MAX_PASSES) passes = DECODE_CORE_MAX_PASSES;
    core->passes = passes;
    core->pass = 0;
    core->found = 0;
    core->seen = 0;
    core->spent = 0;
    core->tried = 0;
    core->cut = 0;
    core->iterations = 0;
    core->redone = 0;
    core->pass_subs = 0;
    decode_retry_reset(&core->retry);
    memset(core->decoded_used, 0, sizeof(core->decoded_used));
    // Per-frequency noise, before any subtraction (decode.md, SNR).
    return decode_snr_prepare(&core->snr, monitor, block_gain);
}

int decode_core_search(decode_core_t* core)
{
    int max = core->config->max_candidates;
    if (max > DECODE_MAX_CANDIDATES) max = DECODE_MAX_CANDIDATES;
    int want = max;
    if (core->subtract) {
        want = max / (core->pass + 1);
        if (want < 1) want = 1;
    }
    core->seen = 0;
    core->pass_subs = 0;
    core->spent += core->config->cost_search;
    core->found = ftx_find_candidates(&core->monitor->wf, want, core->candidates, core->config->min_score);
    return core->found;
}

static float candidate_hz(const decode_core_t* core, const ftx_candidate_t* c)
{
    const monitor_t* m = core->monitor;
    return (m->min_bin + c->freq_offset + c->freq_sub / (float)m->wf.freq_osr) / m->symbol_period;
}

int decode_core_skip(const decode_core_t* core, int i)
{
    if (!core->subtract) return 0;
    const ftx_candidate_t* c = &core->candidates[i];
    // The other freq_osr subdivision of a signal just decoded is the same station: subtracting it again
    // would dig the hole twice in this search.
    if (decode_subtract_already(core->seen_hz, core->seen, candidate_hz(core, c))) return 1;
    return decode_retry_unchanged(&core->retry, c, core->n_tones);
}

// 1 when the budget has room for this many more units, or there is no budget.
static int fits(const decode_core_t* core, int units)
{
    const int budget = core->config->budget_units;
    return budget <= 0 || core->spent + units <= budget;
}

static int worst_candidate(const decode_core_t* core)
{
    return core->config->cost_candidate + core->config->ldpc_iterations;
}

int decode_core_afford(decode_core_t* core)
{
    if (fits(core, worst_candidate(core))) return 1;
    core->cut = 1;
    return 0;
}

void decode_core_decode(const decode_core_t* core, int i, decode_core_attempt_t* attempt)
{
    ftx_decode_status_t status;
    memset(&attempt->message, 0, sizeof(attempt->message));
    memset(&status, 0, sizeof(status));
    attempt->stamp = core->pass_subs;
    attempt->ok = ftx_decode_candidate(&core->monitor->wf, &core->candidates[i], core->config->ldpc_iterations,
                                       &attempt->message, &status)
                      ? 1
                      : 0;
    attempt->iterations = status.ldpc_iterations;
}

// 1 if the payload is new this slot, and remembers it. A full table remembers nothing more, and a
// message it cannot hold is treated as heard: subtracted, not listed.
static int remember(decode_core_t* core, const ftx_message_t* message)
{
    int index = (int)(message->hash % DECODE_CORE_HASH_SLOTS);
    for (int probe = 0; probe < DECODE_CORE_HASH_SLOTS; ++probe) {
        if (!core->decoded_used[index]) {
            core->decoded[index] = *message;
            core->decoded_used[index] = 1;
            return 1;
        }
        const ftx_message_t* held = &core->decoded[index];
        if (held->hash == message->hash && memcmp(held->payload, message->payload, sizeof(message->payload)) == 0) {
            return 0;
        }
        index = (index + 1) % DECODE_CORE_HASH_SLOTS;
    }
    return 0;
}

// 1 when a subtraction committed since `stamp` lowered bins candidate i reads: both span n_tones bins from
// their freq_offset, the rule decode_retry_unchanged() uses.
static int stale(const decode_core_t* core, int i, int stamp)
{
    const int bin = core->candidates[i].freq_offset;
    for (int k = stamp; k < core->pass_subs; ++k) {
        int distance = bin - core->pass_bins[k];
        if (distance < 0) distance = -distance;
        if (distance < core->n_tones) return 1;
    }
    return 0;
}

decode_core_result_t decode_core_commit(decode_core_t* core, int i, const decode_core_attempt_t* ahead,
                                        decode_core_message_t* message)
{
    const ftx_candidate_t* c = &core->candidates[i];
    decode_core_attempt_t again;
    const decode_core_attempt_t* attempt = ahead;
    if (stale(core, i, ahead->stamp)) {
        decode_core_decode(core, i, &again);
        attempt = &again;
        core->redone++;
    }
    core->spent += core->config->cost_candidate + attempt->iterations;
    core->iterations += attempt->iterations;
    core->tried++;
    if (!attempt->ok) {
        decode_retry_note_failed(&core->retry, c);
        return DECODE_CORE_FAILED;
    }
    const int fresh = remember(core, &attempt->message);

    ftx_message_offsets_t offsets;
    memset(message->text, 0, sizeof(message->text));
    if (ftx_message_decode(&attempt->message, core->config->hash_if, message->text, &offsets) != FTX_MESSAGE_RC_OK ||
        message->text[0] == '\0') {
        return DECODE_CORE_UNREADABLE;
    }
    const monitor_t* m = core->monitor;
    message->freq_hz = candidate_hz(core, c);
    message->time_s = (c->time_offset + c->time_sub / (float)m->wf.time_osr) * m->symbol_period;
    // Before this message is subtracted: the SNR reads its tones.
    message->snr = decode_snr_message(&core->snr, m, c, attempt->message.payload);
    message->standard = ftx_message_get_type(&attempt->message) == FTX_MESSAGE_TYPE_STANDARD;

    if (core->subtract) {
        if (core->seen < DECODE_MAX_CANDIDATES) core->seen_hz[core->seen++] = message->freq_hz;
        decode_subtract_message(&core->monitor->wf, c, attempt->message.payload, core->config->subtract_neighbors,
                                core->config->subtract_fraction);
        decode_retry_note_subtracted(&core->retry, c);
        // A search commits at most its candidates, so this never fills.
        if (core->pass_subs < DECODE_MAX_CANDIDATES) core->pass_bins[core->pass_subs++] = (int16_t)c->freq_offset;
    }
    return fresh ? DECODE_CORE_NEW : DECODE_CORE_REPEAT;
}

int decode_core_next_pass(decode_core_t* core)
{
    // Nothing subtracted leaves the waterfall as it was, so another search would only retry what just failed.
    if (core->cut || !core->subtract || core->seen == 0 || core->pass + 1 >= core->passes) return 0;
    if (!fits(core, core->config->cost_search + worst_candidate(core))) {
        core->cut = 1;
        return 0;
    }
    core->pass++;
    return 1;
}
