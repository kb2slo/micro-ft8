#include "ft8_slot_deficit.h"

int64_t ft8_slot_sample_deficit(int64_t samples_got, int64_t slots_crossed,
                                int slot_ms, int sample_rate)
{
    if (slots_crossed < 1 || slot_ms < 1 || sample_rate < 1 || samples_got < 0) {
        return 0;
    }
    const int64_t expected =
        slots_crossed * (int64_t)slot_ms * (int64_t)sample_rate / 1000;
    if (samples_got >= expected) return 0;
    return expected - samples_got;
}
