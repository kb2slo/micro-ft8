#include <cstdio>

#include "decode_sync.h"

static int g_fails = 0;

static void expect_int(int got, int want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, got, want);
        ++g_fails;
    }
}

int main()
{
    // The two halves of the cycle read on 15 m (2026-10-05): one slot's audio began
    // ~0.4 s late, the next began with the last slot's backlog and read ~+1.7 s.
    const float late[] = {-0.48f, -0.39f, -0.48f, -0.48f, -0.39f, -0.48f, -0.32f, -0.48f};
    const float backlog[] = {1.76f, 1.68f, 1.68f, 1.52f, 1.68f, 1.68f, 2.00f, 1.59f};
    expect_int(decode_sync_step_ms(late, 8, 1), 320, "a clean slot 0.48 s late steps forward, capped");
    expect_int(decode_sync_step_ms(backlog, 8, 1), -320, "clean, +1.7 s steps back, capped");
    expect_int(decode_sync_step_ms(backlog, 8, 0), 0, "a slot that began on a backlog steps nothing");
    expect_int(decode_sync_step_ms(late, 8, 0), 0, "an unclean slot steps nothing");

    const float aligned[] = {-0.08f, -0.08f, -0.16f, 0.00f, -0.16f, -0.16f, 0.56f};
    expect_int(decode_sync_step_ms(aligned, 7, 1), 0, "a median within 0.3 s is no step");
    const float small[] = {-0.32f, -0.31f, -0.33f, -0.31f};
    expect_int(decode_sync_step_ms(small, 4, 1), 310, "a step under the cap is the median");
    expect_int(decode_sync_step_ms(small, 3, 1), 0, "three decodes are too few");
    // The median is the middle of the sorted times, not their mean.
    const float skew[] = {-0.40f, 5.0f, -0.40f, 9.0f, -0.40f};
    expect_int(decode_sync_step_ms(skew, 5, 1), 320, "outliers do not move the median");

    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    // Decode times show the phase: clean, four or more, median within 0.5 s.
    expect_int(decode_sync_phase_good(aligned, 7, 1), 1, "aligned decodes: the phase is good");
    expect_int(decode_sync_phase_good(aligned, 7, 0), 0, "not from an unclean slot");
    expect_int(decode_sync_phase_good(backlog, 8, 1), 0, "not 1.7 s off");
    expect_int(decode_sync_phase_good(small, 3, 1), 0, "not from three decodes");

    // A browser clock set (time.md, Browser sync over Port A).
    int64_t held = 0, apply = 0;
    expect_int(clock_set_decide(0, -31000, 1, &held, &apply), CLOCK_SET_APPLY, "an unset clock takes any set");
    expect_int((int)apply, -31000, "as sent");
    expect_int(clock_set_decide(1, 1500, 0, &held, &apply), CLOCK_SET_IGNORE, "within 2 s: nothing");
    expect_int(clock_set_decide(1, -31000, 1, &held, &apply), CLOCK_SET_DISREGARD,
               "2026-11-07: 31 s back with decodes on the slots is disregarded");
    expect_int((int)held, 0, "and nothing is held");
    expect_int(clock_set_decide(1, 30200, 1, &held, &apply), CLOCK_SET_APPLY, "two slots forward, phase good");
    expect_int((int)apply, 30000, "applied as exactly two slots");
    expect_int(clock_set_decide(1, -30100, 1, &held, &apply), CLOCK_SET_HOLD, "two slots back is held");
    expect_int((int)held, -30000, "held as exactly two slots");
    expect_int(clock_set_decide(1, 300, 1, &held, &apply), CLOCK_SET_DROP, "the next set agrees with the clock: dropped");
    expect_int((int)held, 0, "nothing held after a drop");
    expect_int(clock_set_decide(1, -45000, 0, &held, &apply), CLOCK_SET_HOLD, "no decodes, 45 s back: held");
    expect_int(clock_set_decide(1, -44000, 0, &held, &apply), CLOCK_SET_APPLY, "the next asks the same: applied");
    expect_int((int)apply, -44000, "the step the confirming set asked");
    expect_int(clock_set_decide(1, -45000, 0, &held, &apply), CLOCK_SET_HOLD, "held again");
    expect_int(clock_set_decide(1, -20000, 0, &held, &apply), CLOCK_SET_HOLD, "a different step replaces it");
    expect_int((int)held, -20000, "the newer step is held");
    expect_int(clock_set_decide(1, 90000, 0, &held, &apply), CLOCK_SET_APPLY, "no decodes, forward: applied");
    expect_int((int)held, 0, "and the held step cleared");
    return 0;
}
