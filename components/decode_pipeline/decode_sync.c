#include <math.h>

#include "decode_sync.h"

#define DECODE_SYNC_MAX_TIMES 64

static float median_of(const float* times_s, int n)
{
    if (n > DECODE_SYNC_MAX_TIMES) n = DECODE_SYNC_MAX_TIMES;
    float sorted[DECODE_SYNC_MAX_TIMES];
    for (int i = 0; i < n; ++i) {
        float key = times_s[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }
    return sorted[n / 2];
}

int decode_sync_phase_good(const float* times_s, int n, int capture_clean)
{
    return capture_clean && n > 3 && fabsf(median_of(times_s, n)) < 0.5f;
}

#define CLOCK_SET_GATE_MS 2000
#define CLOCK_SET_SLOT_MS 15000
#define CLOCK_SET_SLOT_SLACK_MS 500

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

clock_set_action_t clock_set_decide(int clock_set, int64_t step_ms, int phase_good, int64_t* held_step_ms,
                                    int64_t* apply_step_ms)
{
    *apply_step_ms = 0;
    if (!clock_set) {
        *held_step_ms = 0;
        *apply_step_ms = step_ms;
        return CLOCK_SET_APPLY;
    }
    if (abs64(step_ms) < CLOCK_SET_GATE_MS) {
        if (*held_step_ms != 0) {
            *held_step_ms = 0;
            return CLOCK_SET_DROP;
        }
        return CLOCK_SET_IGNORE;
    }
    int64_t step = step_ms;
    if (phase_good) {
        const int64_t slots = (step_ms >= 0 ? step_ms + CLOCK_SET_SLOT_MS / 2 : step_ms - CLOCK_SET_SLOT_MS / 2) /
                              CLOCK_SET_SLOT_MS;
        if (abs64(step_ms - slots * CLOCK_SET_SLOT_MS) > CLOCK_SET_SLOT_SLACK_MS) return CLOCK_SET_DISREGARD;
        step = slots * CLOCK_SET_SLOT_MS;
    }
    if (step < 0) {
        if (*held_step_ms != 0 && abs64(step - *held_step_ms) < CLOCK_SET_GATE_MS) {
            *held_step_ms = 0;
            *apply_step_ms = step;
            return CLOCK_SET_APPLY;
        }
        *held_step_ms = step;
        return CLOCK_SET_HOLD;
    }
    *held_step_ms = 0;
    *apply_step_ms = step;
    return CLOCK_SET_APPLY;
}

int decode_sync_step_ms(const float* times_s, int n, int capture_clean)
{
    if (!capture_clean || n <= 3) return 0;
    float median = median_of(times_s, n);
    if (fabsf(median) <= 0.3f) return 0;
    int delta_ms = (int)lrintf(-median * 1000.0f);
    if (delta_ms > 320) delta_ms = 320;
    if (delta_ms < -320) delta_ms = -320;
    return delta_ms;
}
