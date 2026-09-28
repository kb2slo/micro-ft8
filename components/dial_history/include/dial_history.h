#pragma once

// What the radio's dial was set to, and from when: every successful tune is
// noted (radio_control_sync_frequency_mode()), and a decode asks what the
// dial held at the middle of its signal (decode_pipeline), so the Port A
// DECODE event can say which frequency it was heard on even when the
// operator retunes partway through a slot. Times are rtc_now_ms() (UTC ms),
// the clock the slot loop counts slots in.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tunes remembered; older ones are forgotten. A slot sees at most a few.
#define DIAL_HISTORY_LEN 8

// The dial was set to `hz` at `at_ms`. A repeat of the current frequency is
// not a change and keeps the earlier time.
void dial_history_note(int64_t at_ms, uint32_t hz);

// The dial frequency in effect at `at_ms`: the latest tune at or before it.
// 0 when nothing on record is that old.
uint32_t dial_history_at(int64_t at_ms);

// Forgets every tune (host tests).
void dial_history_reset(void);

#ifdef __cplusplus
}
#endif
