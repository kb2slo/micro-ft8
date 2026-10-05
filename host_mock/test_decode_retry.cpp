#include <cstdio>

extern "C" {
#include "decode_retry.h"
}

static int g_fails = 0;

static void expect_int(int got, int want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, got, want);
        ++g_fails;
    }
}

static ftx_candidate_t cand(int time_offset, int time_sub, int freq_offset, int freq_sub)
{
    ftx_candidate_t c = {};
    c.time_offset = (int16_t)time_offset;
    c.time_sub = (uint8_t)time_sub;
    c.freq_offset = (int16_t)freq_offset;
    c.freq_sub = (uint8_t)freq_sub;
    return c;
}

int main()
{
    static decode_retry_t retry;
    decode_retry_reset(&retry);
    const ftx_candidate_t failed = cand(3, 1, 100, 0);

    expect_int(decode_retry_unchanged(&retry, &failed, 8), 0, "never tried is decoded");
    decode_retry_note_failed(&retry, &failed);
    expect_int(decode_retry_unchanged(&retry, &failed, 8), 1, "failed, nothing subtracted: skip");

    // Same bins, another subdivision or start: a different set of cells.
    ftx_candidate_t other = cand(3, 0, 100, 0);
    expect_int(decode_retry_unchanged(&retry, &other, 8), 0, "other time_sub is not the same cells");
    other = cand(3, 1, 100, 1);
    expect_int(decode_retry_unchanged(&retry, &other, 8), 0, "other freq_sub is not the same cells");
    other = cand(4, 1, 100, 0);
    expect_int(decode_retry_unchanged(&retry, &other, 8), 0, "other start block is not the same cells");

    // FT8 spans bins 100..107. A signal at 92 spans 92..99 and does not touch it.
    const ftx_candidate_t below = cand(0, 0, 92, 0);
    decode_retry_note_subtracted(&retry, &below);
    expect_int(decode_retry_unchanged(&retry, &failed, 8), 1, "a signal one bin clear is no change");
    const ftx_candidate_t above = cand(0, 0, 108, 1);
    decode_retry_note_subtracted(&retry, &above);
    expect_int(decode_retry_unchanged(&retry, &failed, 8), 1, "a signal above, one bin clear, is no change");
    const ftx_candidate_t overlap = cand(10, 0, 93, 1);
    decode_retry_note_subtracted(&retry, &overlap);
    expect_int(decode_retry_unchanged(&retry, &failed, 8), 0, "one shared bin means retry");

    // FT4 spans 4 bins, so 96 (96..99) is clear of 100 and 97 is not.
    decode_retry_reset(&retry);
    decode_retry_note_failed(&retry, &failed);
    const ftx_candidate_t ft4_clear = cand(0, 0, 96, 0);
    decode_retry_note_subtracted(&retry, &ft4_clear);
    expect_int(decode_retry_unchanged(&retry, &failed, 4), 1, "FT4: four bins clear is no change");
    const ftx_candidate_t ft4_touch = cand(0, 0, 97, 0);
    decode_retry_note_subtracted(&retry, &ft4_touch);
    expect_int(decode_retry_unchanged(&retry, &failed, 4), 0, "FT4: a shared bin means retry");

    // A subtraction the list could not hold may have touched any bin.
    decode_retry_reset(&retry);
    decode_retry_note_failed(&retry, &failed);
    const ftx_candidate_t far = cand(0, 0, 400, 0);
    for (int i = 0; i < DECODE_MAX_CANDIDATES; ++i) decode_retry_note_subtracted(&retry, &far);
    expect_int(decode_retry_unchanged(&retry, &failed, 8), 1, "a full list of far signals is no change");
    decode_retry_note_subtracted(&retry, &far);
    expect_int(decode_retry_unchanged(&retry, &failed, 8), 0, "one past the list means retry");

    // A failed list that is full retries what it could not record.
    decode_retry_reset(&retry);
    for (int i = 0; i < DECODE_MAX_CANDIDATES; ++i) {
        const ftx_candidate_t c = cand(i, 0, 0, 0);
        decode_retry_note_failed(&retry, &c);
    }
    const ftx_candidate_t unrecorded = cand(DECODE_MAX_CANDIDATES, 0, 0, 0);
    decode_retry_note_failed(&retry, &unrecorded);
    expect_int(decode_retry_unchanged(&retry, &unrecorded, 8), 0, "an unrecorded failure is retried");

    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
