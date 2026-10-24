// rtc_core: the soft clock is one atomic offset from uptime
// (components/rtc_core/include/rtc_core.h, docs/architecture/time.md). Pins
// the arithmetic and an unset clock, and, from two threads at once: steps
// against steps lose none, a read never sees half a set, and a step never
// writes over a set.

#include <sys/time.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "rtc_core.h"

// esp_timer.h's mock reads this as uptime, in ms.
static int64_t s_uptime_ms = 0;
int64_t mock_clock_get() { return s_uptime_ms; }

// The Makefile builds rtc_core.cpp with settimeofday renamed to this, so the
// test never sets this machine's clock.
static std::atomic<int> s_mirrors{0};
static std::atomic<long long> s_mirror_secs{0};
extern "C" int rtc_test_settimeofday(const struct timeval* tv, const void* /*tz*/) {
    s_mirror_secs = (long long)tv->tv_sec;
    ++s_mirrors;
    return 0;
}

static int g_fails = 0;

static void expect(long long got, long long want, const char* msg) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %lld want %lld)\n", msg, got, want);
        ++g_fails;
    }
}

int main() {
    // Unset: the clock reads uptime, stamps read 0, and a step does nothing.
    s_uptime_ms = 5000;
    expect(rtc_is_set(), false, "unset at boot");
    expect(rtc_now_ms(), 5000, "unset reads uptime");
    expect(rtc_current_epoch_seconds(), 5, "unset seconds are uptime seconds");
    expect(rtc_epoch_secs_or_zero(), 0, "unset stamps are 0");
    rtc_apply_decode_sync(300);
    expect(rtc_now_ms(), 5000, "a step does not move an unset clock");
    expect(rtc_is_set(), false, "a step does not set the clock");
    expect(s_mirrors.load(), 0, "an unset clock is not mirrored");

    // A set from the phone at uptime 10000, its millisecond 250, as
    // porta_host_set_clock() passes it: ms_start is uptime minus the millis.
    // 1789012335 is the start of FT8 slot 119267489.
    s_uptime_ms = 10000;
    rtc_seed_epoch((time_t)1789012335, 10000 - 250);
    expect(rtc_is_set(), true, "set");
    expect(rtc_now_ms(), 1789012335250LL, "set reads the epoch plus its millis");
    s_uptime_ms = 11000;
    expect(rtc_now_ms(), 1789012336250LL, "the clock runs with uptime");
    expect(rtc_current_epoch_seconds(), 1789012336LL, "whole seconds");
    expect(rtc_epoch_secs_or_zero(), 1789012336LL, "stamps once set");

    // A decode step moves it by delta and mirrors the result.
    rtc_apply_decode_sync(320);
    expect(rtc_now_ms(), 1789012336570LL, "a +320 ms step");
    rtc_apply_decode_sync(-150);
    expect(rtc_now_ms(), 1789012336420LL, "a -150 ms step");
    expect(s_mirrors.load(), 2, "each step mirrors");
    expect(s_mirror_secs.load(), 1789012336LL, "the mirror holds the stepped time");

    // A later set replaces the clock whole, earlier steps included.
    rtc_seed_epoch((time_t)1789012400, 11000);
    expect(rtc_now_ms(), 1789012400000LL, "a later set replaces the clock");

    // Steps from two threads at once: +1 and -1 the same number of times. A
    // plain read-modify-write loses some of them; the clock must come back.
    // ESP_LOGI prints on every mirror, so stdout goes quiet for this part.
    fflush(stdout);
    if (!freopen("/dev/null", "w", stdout)) {
        fprintf(stderr, "FAIL: could not silence stdout\n");
        return 1;
    }
    // Both wait on one gate, so they run at the same time, not one after the other.
    // Counts sized so this test failed 10 runs of 10 against the clock as
    // three plain globals (a Mac, 2026-10-23); under half a second here.
    const int kSteps = 1000000;
    std::atomic<int> ready{0};
    auto stepper = [&](int delta) {
        ++ready;
        while (ready.load() < 2) {
        }
        for (int i = 0; i < kSteps; ++i) rtc_apply_decode_sync(delta);
    };
    std::thread up(stepper, +1);
    std::thread down(stepper, -1);
    up.join();
    down.join();
    expect(rtc_now_ms(), 1789012400000LL, "steps from two threads all land");

    // A set racing reads: every read is one whole set or the other, never a
    // mix of the two. The sets differ in both epoch and start, so a read of
    // one set's epoch with the other's start reads neither. Reading starts
    // once the setter is running. Uptime is 11000.
    const time_t a_s = 1789012400;
    const time_t b_s = 1789099999;
    const int64_t a_ms = (int64_t)a_s * 1000;         // set at start 11000
    const int64_t b_ms = (int64_t)b_s * 1000 - 1000;  // set at start 12000
    std::atomic<bool> stop{false};
    std::atomic<bool> setting{false};
    std::thread setter([&] {
        for (int i = 0; !stop.load(); ++i) {
            if (i & 1) {
                rtc_seed_epoch(b_s, 12000);
            } else {
                rtc_seed_epoch(a_s, 11000);
            }
            setting = true;
        }
    });
    while (!setting.load()) {
    }
    int torn = 0;
    for (int i = 0; i < 10000000; ++i) {
        const int64_t now = rtc_now_ms();
        if (now != a_ms && now != b_ms) ++torn;
    }
    stop = true;
    setter.join();
    expect(torn, 0, "no read sees half a set");

    // A set racing steps, the pair production has (the phone's set on the
    // main loop, auto-sync on the decode task). A step that read the clock
    // before a set must not write over it: right after each set the clock
    // reads that set, give or take the steps made since, which sum to -1, 0
    // or +1 because the stepper alternates them.
    // Sets run until the stepper has made kRaceSteps steps, so the two
    // overlap for the whole run, not just its start.
    const int kRaceSteps = 500000;
    std::atomic<bool> stop_steps{false};
    std::atomic<int> steps_done{0};
    std::thread stepper2([&] {
        for (int i = 0; !stop_steps.load(); ++i) {
            rtc_apply_decode_sync((i & 1) ? -1 : +1);
            ++steps_done;
        }
    });
    while (steps_done.load() == 0) {
    }
    int lost_sets = 0;
    for (int i = 0; steps_done.load() < kRaceSteps; ++i) {
        const bool b = (i & 1) != 0;
        if (b) {
            rtc_seed_epoch(b_s, 12000);
        } else {
            rtc_seed_epoch(a_s, 11000);
        }
        const int64_t want = b ? b_ms : a_ms;
        const int64_t got = rtc_now_ms();
        if (got < want - 1 || got > want + 1) ++lost_sets;
    }
    stop_steps = true;
    stepper2.join();
    expect(lost_sets, 0, "a step never writes over a set");

    if (g_fails) {
        fprintf(stderr, "rtc_core: %d failure(s)\n", g_fails);
        return 1;
    }
    fprintf(stderr, "PASS: rtc_core\n");
    return 0;
}
