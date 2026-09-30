#pragma once

// Sweeps SWR over frequency ranges through the QMX's SWR Tune mode (RFC 0010,
// docs/architecture/radio.md "QMX SWR Tune over CAT"). The caller hands over ranges
// and calls swr_sweep_tick() often; nothing here blocks, so USB, audio and Port A keep
// running while a sweep is in progress. Events come back through a callback.
//
// Per point: RX;, MD6;, FA<hz>;, verify with FA;, MD8; (which keys by itself), discard
// the first 300 ms, median of three SW; reads, verify FA; still holds. The radio refuses
// FA outside the bands it is configured for, so a range whose first and last frequency
// are both refused is reported as skipped and never keys. However a sweep ends (done,
// cancelled, CAT lost) the radio is unkeyed and returned to the operating frequency.
//
// Not thread-safe: start, tick and cancel must be called from one task. A caller on another
// task (a Port A handler) sets a flag the ticking task turns into swr_sweep_cancel(). The
// caller must also tick at least every 100 ms while a sweep runs: if ticks stop while the
// radio is keyed, only the radio's own CAT timeout (120 s on the bench unit) un-keys it.

#include <stddef.h>
#include <stdint.h>

#include "radio_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWR_SWEEP_MAX_RANGES 12
#define SWR_SWEEP_MAX_POINTS 200
// Seconds a point costs, for the estimate a page shows: measured 0.73 s on a QMX, plus margin.
#define SWR_SWEEP_POINT_MS 800

typedef struct {
    uint32_t lo_khz;
    uint32_t hi_khz;
    uint32_t step_khz;
} swr_sweep_range_t;

typedef enum {
    SWR_SWEEP_EVENT_POINT,           // freq_hz, swr_hundredths
    SWR_SWEEP_EVENT_BAND_SKIPPED,    // the radio refused both edges of the range
    SWR_SWEEP_EVENT_DONE,
    SWR_SWEEP_EVENT_ABORTED,         // abort_reason
} swr_sweep_event_kind_t;

typedef enum {
    SWR_SWEEP_ABORT_CANCELLED,
    SWR_SWEEP_ABORT_CAT_LOST,        // a CAT call failed, or three queries in a row went unanswered
} swr_sweep_abort_reason_t;

typedef struct {
    swr_sweep_event_kind_t kind;
    uint8_t range_index;
    uint32_t freq_hz;
    int swr_hundredths;
    swr_sweep_abort_reason_t abort_reason;
} swr_sweep_event_t;

typedef void (*swr_sweep_event_fn)(const swr_sweep_event_t* event, void* user);

typedef enum {
    SWR_SWEEP_START_OK,
    SWR_SWEEP_START_BAD_RANGE,
    SWR_SWEEP_START_TOO_MANY,
    SWR_SWEEP_START_BUSY,
    SWR_SWEEP_START_UNSUPPORTED,     // io is null: the selected radio cannot sweep
} swr_sweep_start_result_t;

// Points a plan measures, or 0 when any range is invalid or there are too many ranges.
// Each range is lo, lo+step, ... and always ends on hi.
uint32_t swr_sweep_point_count(const swr_sweep_range_t* ranges, size_t count);

// Milliseconds to show the operator, from the point count.
uint32_t swr_sweep_estimate_ms(const swr_sweep_range_t* ranges, size_t count);

// Starts a sweep. `restore_hz` is the operating frequency to return to. The ranges are copied.
swr_sweep_start_result_t swr_sweep_start(const swr_sweep_range_t* ranges, size_t count, uint32_t restore_hz,
                                         const radio_swr_io_t* io, swr_sweep_event_fn on_event, void* user,
                                         int64_t now_ms);

// One step of the sweep, at most one radio command per call. Cheap when idle.
void swr_sweep_tick(int64_t now_ms);

// De-keys the radio now, then restores it and reports ABORTED (cancelled) on the next tick.
void swr_sweep_cancel(void);

int swr_sweep_active(void);

#ifdef __cplusplus
}
#endif
