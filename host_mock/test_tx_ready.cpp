// tx_ready: when Call CQ and a reply are refused (components/radio_control/include/tx_ready.h).

#include <cstdio>
#include <cstring>

#include "tx_ready.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static bool says(const char* got, const char* want)
{
    return got && std::strcmp(got, want) == 0;
}

int main()
{
    // CAT up, audio arriving, the decoder running: nothing is refused.
    expect(tx_ready_refusal(true, true, false) == nullptr, "a ready radio is not refused");

    // No radio at all: the case that queued a CQ which keyed when a radio was plugged in later.
    expect(says(tx_ready_refusal(false, false, false), "No radio"), "no CAT and no audio: No radio");
    expect(says(tx_ready_refusal(false, false, true), "No radio"), "No radio comes before Not decoding");

    // Half up: the device is there and still syncing, or the HL2, which has no CAT.
    expect(says(tx_ready_refusal(false, true, false), "CAT not ready"), "audio without CAT");
    expect(says(tx_ready_refusal(true, false, false), "no audio from the radio"), "CAT without audio");

    // The radio is fine and the decoder could not start: nothing would be heard in answer.
    expect(says(tx_ready_refusal(true, true, true), "Not decoding"), "the decoder not running");

    if (g_fails) return 1;
    printf("PASS: Call CQ and a reply are refused unless the radio is ready\n");
    return 0;
}
