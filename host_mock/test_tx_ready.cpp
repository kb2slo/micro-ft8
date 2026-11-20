// tx_ready: when Call CQ and a reply are refused, and which calls we may transmit under at all
// (components/radio_control/include/tx_ready.h).

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
    expect(tx_ready_refusal(true, true, true, false) == nullptr, "a ready radio is not refused");

    // No radio at all: the case that queued a CQ which keyed when a radio was plugged in later.
    expect(says(tx_ready_refusal(true, false, false, false), "No radio"), "no CAT and no audio: No radio");
    expect(says(tx_ready_refusal(true, false, false, true), "No radio"), "No radio comes before Not decoding");

    // Half up: the device is there and still syncing, or the HL2, which has no CAT.
    expect(says(tx_ready_refusal(true, false, true, false), "CAT not ready"), "audio without CAT");
    expect(says(tx_ready_refusal(true, true, false, false), "no audio from the radio"), "CAT without audio");

    // The radio is fine and the decoder could not start: nothing would be heard in answer.
    expect(says(tx_ready_refusal(true, true, true, true), "Not decoding"), "the decoder not running");

    // No callsign: refused before anything about the radio, which is fine or not either way.
    expect(says(tx_ready_refusal(false, true, true, false), "No callsign set"), "a ready radio and no call");
    expect(says(tx_ready_refusal(false, false, false, false), "No callsign set"), "the call is said before No radio");

    // Which calls we may transmit under: a new station's placeholder and NOCALL are not calls.
    expect(!tx_call_ok("YOURCALL"), "a new station's YOURCALL");
    expect(!tx_call_ok("NOCALL"), "NOCALL");
    expect(!tx_call_ok(""), "an empty call");
    expect(!tx_call_ok(nullptr), "no call");
    expect(!tx_call_ok("KB"), "too short");
    expect(!tx_call_ok("AB1CDEFGHIJKLM"), "fourteen characters: too long");
    expect(!tx_call_ok("KBSLO"), "no digit");
    expect(!tx_call_ok("12345"), "no letter");
    expect(!tx_call_ok("KB2 SLO"), "a space");
    expect(!tx_call_ok("kb2slo"), "lower case: the station keeps calls upper case (trim_upper_copy())");
    expect(tx_call_ok("KB2SLO"), "a call");
    expect(tx_call_ok("K1A"), "three characters");
    expect(tx_call_ok("W1AW/P"), "a portable suffix");
    expect(tx_call_ok("VP2E/KB2SLO"), "a prefix, 11 characters");

    if (g_fails) return 1;
    printf("PASS: Call CQ and a reply are refused unless the call is set and the radio is ready\n");
    return 0;
}
