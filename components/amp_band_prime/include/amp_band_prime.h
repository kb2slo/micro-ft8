#pragma once

// Briefly keys QMX SWR Tune after an operator-requested band change so an
// RF-sensing amplifier can select its band before the next FT8 transmission.
// The caller requests work from any task; the main loop owns the CAT calls.

#include <stdint.h>

#include "radio_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMP_BAND_PRIME_PULSE_MS 500

typedef enum {
    AMP_BAND_PRIME_DONE,
    AMP_BAND_PRIME_ABORTED,
} amp_band_prime_event_kind_t;

typedef enum {
    AMP_BAND_PRIME_ABORT_CANCELLED,
    AMP_BAND_PRIME_ABORT_CAT_LOST,
    AMP_BAND_PRIME_ABORT_STALLED,
} amp_band_prime_abort_reason_t;

typedef struct {
    amp_band_prime_event_kind_t kind;
    amp_band_prime_abort_reason_t abort_reason;
} amp_band_prime_event_t;

typedef void (*amp_band_prime_event_fn)(const amp_band_prime_event_t* event, void* user);

typedef enum {
    AMP_BAND_PRIME_START_OK,
    AMP_BAND_PRIME_START_BUSY,
    AMP_BAND_PRIME_START_UNSUPPORTED,
} amp_band_prime_start_result_t;

// Any task: hands a request to the task calling amp_band_prime_tick(). `io`
// must remain valid for the run (radio_control_swr_io() is process-lifetime).
amp_band_prime_start_result_t amp_band_prime_request_start(
    const radio_swr_io_t* io, amp_band_prime_event_fn on_event, void* user);

// Any task: ends a pending or active pulse. An active pulse is de-keyed by the
// next tick.
void amp_band_prime_request_cancel(void);

// True from an accepted request until MD8 has been followed by RX and MD6.
int amp_band_prime_busy(void);
int amp_band_prime_active(void);

// No band primed yet, as at boot.
#define AMP_BAND_PRIME_NONE (-1)

// Whether a transmission due on `band` must prime first: priming is on and `band` is not the one last
// primed or tried (`primed_band`). A band set while the radio was away (at boot, or with CAT down) was
// never primed by its band change, so its first transmission primes it (docs/architecture/radio.md,
// Prime an RF-sensing amplifier on a band change).
int amp_band_prime_needed(int enabled, int primed_band, int band);

// One state-machine step, issuing at most one CAT command. Tick at least every
// 100 ms. A tick delayed by over a second while keyed de-keys and aborts.
void amp_band_prime_tick(int64_t now_ms);

#ifdef __cplusplus
}
#endif
