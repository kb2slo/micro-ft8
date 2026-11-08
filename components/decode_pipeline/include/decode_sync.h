#pragma once

#include <stdint.h>

// The soft-clock step a decoded slot asks for, from the median start time of its
// messages. A slot whose capture did not start on its boundary, or lost samples,
// measures the capture and not the clock, so it asks for none
// (docs/architecture/decode.md, **The time budget**).

#ifdef __cplusplus
extern "C" {
#endif

// times_s holds n decode start times in the recording, in seconds. Returns ms to
// step the clock forward (positive) or back, or 0. Fewer than four decodes, a
// median within 0.3 s, or an unclean capture is 0; a step is at most 320 ms.
int decode_sync_step_ms(const float* times_s, int n, int capture_clean);

// True when a decoded slot shows the clock on its slots: a clean capture, four decodes or more, and their
// median start within 0.5 s. Decode times measure the clock's phase within a slot, not which slot it is.
int decode_sync_phase_good(const float* times_s, int n, int capture_clean);

// What a browser clock set does (docs/architecture/time.md, Browser sync over Port A). A set that waited in a
// stalled sidekick carries an old time, so it can only move the clock back: one set it 31 s back on
// 2026-11-07. In order:
//   unset clock                       APPLY, the step as sent;
//   within 2 s                        IGNORE (DROP if a step was held: this set agrees with the clock);
//   phase_good, not a whole slot      DISREGARD: decodes say the phase is right, so only which slot can be;
//                                     a whole slot is 15 s within 0.5 s, and applies exactly;
//   backward                          HOLD it, and APPLY only when the next set asks the same within 2 s;
//   forward                           APPLY: a stale set cannot move the clock forward.
// step_ms is the time sent minus the clock now. *held_step_ms is 0 or the held step, updated in place;
// *apply_step_ms is the step to apply when the result is APPLY.
typedef enum {
    CLOCK_SET_APPLY = 0,
    CLOCK_SET_IGNORE,
    CLOCK_SET_DISREGARD,
    CLOCK_SET_HOLD,
    CLOCK_SET_DROP,
} clock_set_action_t;
clock_set_action_t clock_set_decide(int clock_set, int64_t step_ms, int phase_good, int64_t* held_step_ms,
                                    int64_t* apply_step_ms);

#ifdef __cplusplus
}
#endif
