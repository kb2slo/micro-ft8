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
    return 0;
}
