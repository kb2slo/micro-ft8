// dial_history: the frequency a decode was heard on, from the tunes noted
// before and during its slot (components/dial_history/include/dial_history.h).

#include <cstdio>

#include "dial_history.h"

static int g_fails = 0;

static void expect_hz(uint32_t got, uint32_t want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %u want %u)\n", msg, (unsigned)got, (unsigned)want);
        ++g_fails;
    }
}

int main()
{
    // Slot 119267489 (FT8) starts at 1789012335000 ms.
    const int64_t slot = 1789012335000LL;

    dial_history_reset();
    expect_hz(dial_history_at(slot), 0, "nothing on record reads as unknown");

    dial_history_note(slot - 60000, 7074000);
    expect_hz(dial_history_at(slot + 7000), 7074000, "a tune a minute before holds through the slot");
    expect_hz(dial_history_at(slot - 60001), 0, "before the first tune is unknown");

    // Retuned 3 s into the slot: a signal centred before that was heard on
    // 40 m, one centred after on 20 m.
    dial_history_note(slot + 3000, 14074000);
    expect_hz(dial_history_at(slot + 2999), 7074000, "just before the retune is still 40 m");
    expect_hz(dial_history_at(slot + 3000), 14074000, "the retune's own instant is the new frequency");
    expect_hz(dial_history_at(slot + 7300), 14074000, "a signal centred after the retune is 20 m");

    // A repeat tune (a re-sync) is not a change: the earlier time stands.
    dial_history_note(slot + 9000, 14074000);
    expect_hz(dial_history_at(slot + 4000), 14074000, "a repeat keeps the earlier time");

    // The ring forgets the oldest tunes, never the newest.
    dial_history_reset();
    for (int i = 0; i < DIAL_HISTORY_LEN + 3; ++i) {
        dial_history_note(slot + i * 1000, 14000000 + (uint32_t)i);
    }
    expect_hz(dial_history_at(slot + (DIAL_HISTORY_LEN + 2) * 1000), 14000000 + DIAL_HISTORY_LEN + 2,
              "the newest tune survives a full ring");
    expect_hz(dial_history_at(slot), 0, "a forgotten tune reads as unknown, not as a newer one");

    if (g_fails) {
        fprintf(stderr, "%d FAIL(s)\n", g_fails);
        return 1;
    }
    printf("PASS: dial history\n");
    return 0;
}
