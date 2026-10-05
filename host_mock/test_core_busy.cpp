#include <cstdio>
#include <cstdint>

#include "decode_core_busy.h"

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
    expect_int(decode_core_busy_pct(0, 0), 0, "no elapsed time is not a busy core");
    expect_int(decode_core_busy_pct(0, 2000000), 100, "idle never ran");
    expect_int(decode_core_busy_pct(2000000, 2000000), 0, "idle ran the whole window");
    expect_int(decode_core_busy_pct(2000001, 2000000), 0, "an idle delta past elapsed is 0");
    expect_int(decode_core_busy_pct(500, 1000), 50, "half the window");
    expect_int(decode_core_busy_pct(1, 1000), 99, "truncates toward the idle side");
    // A 2 s decode at the 1 µs counter, core idle for 200 ms.
    expect_int(decode_core_busy_pct(200000, 2000000), 90, "a two-second decode");

    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
