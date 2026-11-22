// The slot our transmission covers is skipped on the channel that would hear it, in that channel's own units,
// and a skipped slot marks the applied index the TX gate reads (ft8_tx_slot.h).
#include <cstdio>
#include <cstdint>

#include "decode_tx_state.h"
#include "ft8_tx_slot.h"
#include "tx_timing.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static void reset()
{
    decode_tx_state_set_tx_slot_idx(-1);
    decode_tx_state_set_tx_ft4_period_idx(-1);
    decode_tx_state_set_decode_applied_slot_idx(-1);
    decode_tx_state_set_ft4_applied_period_idx(-1);
}

// What tx_scheduler.cpp records when a transmission starts in slot_idx of its mode.
static void transmit(bool ft4, int64_t slot_idx)
{
    if (ft4) {
        decode_tx_state_set_tx_ft4_period_idx(slot_idx);
        decode_tx_state_set_tx_slot_idx(tx_ft8_slot_of_ft4_period(slot_idx));
    } else {
        decode_tx_state_set_tx_slot_idx(slot_idx);
    }
}

int main()
{
    // FT4 alone: the station's channel is FT4, counted in 7.5 s periods. The 2026-11-22 bench run decoded
    // our own CQ here, the period compared with the FT8 slot it falls in.
    reset();
    const int64_t p = 239380402;
    transmit(true, p);
    expect(!ft8_tx_slot_is_tx(true, true, p - 1), "FT4 alone: the period before ours decodes");
    expect(ft8_tx_slot_is_tx(true, true, p), "FT4 alone: the period we transmitted in is skipped");
    expect(!ft8_tx_slot_is_tx(true, true, tx_ft8_slot_of_ft4_period(p)),
           "FT4 alone: a period numbered as the FT8 slot is not ours");
    ft8_tx_slot_mark_applied(true, true, p);
    ft8_tx_slot_clear(true, true, p);
    expect(decode_tx_state_get_ft4_applied_period_idx() == p, "FT4 alone: the skip marks FT4's gate applied");
    expect(decode_tx_state_get_tx_ft4_period_idx() == -1, "FT4 alone: the skip clears the period");
    expect(!ft8_tx_slot_is_tx(true, true, p), "FT4 alone: once cleared, the period is not skipped again");

    // FT8 at the station: unchanged, slots compared in FT8's units.
    reset();
    const int64_t s = 119690201;
    transmit(false, s);
    expect(ft8_tx_slot_is_tx(false, true, s), "FT8: the slot we transmitted in is skipped");
    expect(!ft8_tx_slot_is_tx(false, true, s + 1), "FT8: the next slot decodes");
    expect(!ft8_tx_slot_is_tx(true, false, 2 * s), "FT8: the FT4 channel beside it is gated by tx_active, not here");
    ft8_tx_slot_mark_applied(false, true, s);
    ft8_tx_slot_clear(false, true, s);
    expect(decode_tx_state_get_decode_applied_slot_idx() == s, "FT8: the skip marks the station's gate applied");
    expect(decode_tx_state_get_ft4_applied_period_idx() == -1, "FT8: the station's slot leaves FT4's gate alone");
    expect(decode_tx_state_get_tx_slot_idx() == -1, "FT8: the skip clears the slot");

    // FT4 beside FT8: an FT4 transmission deafens its period on the FT4 channel and its FT8 slot on the
    // station's; the other FT4 period in that slot still decodes.
    reset();
    const int64_t q = 239380404;
    transmit(true, q);
    expect(ft8_tx_slot_is_tx(true, false, q), "beside: the FT4 period we transmitted in is skipped");
    expect(!ft8_tx_slot_is_tx(true, false, q + 1), "beside: the other FT4 period in the slot decodes");
    expect(ft8_tx_slot_is_tx(false, true, tx_ft8_slot_of_ft4_period(q)), "beside: the FT8 slot it falls in is skipped");
    ft8_tx_slot_mark_applied(true, false, q);
    expect(decode_tx_state_get_ft4_applied_period_idx() == q, "beside: the FT4 skip marks FT4's gate");
    expect(decode_tx_state_get_decode_applied_slot_idx() == -1, "beside: the FT4 channel leaves the station's mark");
    ft8_tx_slot_clear(true, false, q);
    expect(decode_tx_state_get_tx_ft4_period_idx() == -1, "beside: the FT4 skip clears its period");
    expect(decode_tx_state_get_tx_slot_idx() == tx_ft8_slot_of_ft4_period(q), "beside: and leaves FT8's slot");

    // A mark never goes back.
    ft8_tx_slot_mark_applied(true, false, q - 3);
    expect(decode_tx_state_get_ft4_applied_period_idx() == q, "an older period does not lower the mark");

    if (g_fails) return 1;
    printf("test_tx_slot: all passed\n");
    return 0;
}
