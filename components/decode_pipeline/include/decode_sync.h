#pragma once

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

#ifdef __cplusplus
}
#endif
