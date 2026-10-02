#include <cstdio>
#include <cstdint>

#include "ft8_sample_ring.h"

static int g_fails = 0;

static void expect_u(unsigned got, unsigned want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %u want %u)\n", msg, got, want);
        ++g_fails;
    }
}

int main()
{
    float storage[8];
    Ft8SampleRing ring;
    ft8_sample_ring_init(&ring, storage, 8);

    float in[6] = {1, 2, 3, 4, 5, 6};
    expect_u(ft8_sample_ring_write(&ring, in, 6), 6, "a short write fits");
    expect_u(ft8_sample_ring_count(&ring), 6, "count after write");

    float out[8] = {};
    expect_u(ft8_sample_ring_read(&ring, out, 4), 4, "a partial read");
    expect_u((unsigned)out[0], 1, "first sample");
    expect_u((unsigned)out[3], 4, "fourth sample");
    expect_u(ft8_sample_ring_count(&ring), 2, "two left");

    float more[8] = {7, 8, 9, 10, 11, 12, 13, 14};
    // capacity 8 holds 7. Two are still in, so five fit and three drop.
    expect_u(ft8_sample_ring_write(&ring, more, 8), 5, "write stops at full");
    expect_u(ft8_sample_ring_dropped(&ring), 3, "the rest is counted dropped");
    expect_u(ft8_sample_ring_count(&ring), 7, "full is capacity minus one");

    expect_u(ft8_sample_ring_read(&ring, out, 8), 7, "drain the ring");
    expect_u((unsigned)out[0], 5, "wrap keeps order");
    expect_u((unsigned)out[6], 11, "last stored sample");
    expect_u(ft8_sample_ring_read(&ring, out, 4), 0, "empty read");
    expect_u(ft8_sample_ring_count(&ring), 0, "empty count");

    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
