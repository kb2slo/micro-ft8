#pragma once

// Which receive path the QMX is decoded from, slot by slot (architecture/decode.md, I/Q front end;
// architecture/radio.md, The QMX's receive output). Pure: the main loop decides, the ingest task
// follows, and the host test drives both with made-up times.
//
// The decision for slot s is taken kQmxRxLeadMs before s begins, from the station's qmx_iq and
// qmx_iq_ab as they read then (the QmxRxPlan). The main loop sends `Q9 <n>;` for it at once; the
// ingest task changes what it converts at the boundary itself and resets the front end, so a slot's
// samples all come from one path. A slot that would change the path while a transmission, Tune, sweep
// or prime is running keeps the path it has instead: the radio is not told, and the next slot asks again.

#include <stdbool.h>
#include <stdint.h>

// Q9 goes out this long before the boundary: the radio applies it at once and the USB stream keeps
// every frame (bench, QMX 1_04_010), so what follows the boundary is wholly in the new format, and
// the half second before it, which holds no signal (FT8 starts 0.5 s in), is the margin for the write.
static constexpr int64_t kQmxRxLeadMs = 500;

// The path slot `slot` should be received on. qmx_iq_ab = N > 0: IQ for N slots, audio for N, and so
// on from slot 0, whatever qmx_iq says (N = 2: each path gets an even and an odd slot, so the
// comparison is not between the two parities of the 15-second cycle). Otherwise qmx_iq.
// N must be 1 or even (qmx_iq_ab_valid): an odd N of 3 or more gives one path the even slots more often.
static inline bool qmx_rx_slot_wants_iq(int64_t slot, bool qmx_iq, int ab_slots)
{
    if (ab_slots <= 0) {
        return qmx_iq;
    }
    return ((slot / ab_slots) & 1) == 0;
}

static inline int64_t qmx_rx_slot_of(int64_t ms, int64_t slot_ms)
{
    return ms / slot_ms;
}

struct QmxRxPlan {
    int64_t slot = 0;      // the first slot received on `iq`
    bool iq = false;       // the path from `slot` on
    bool prev_iq = false;  // the path before it
};

static inline QmxRxPlan qmx_rx_plan_make(int64_t slot, bool iq)
{
    QmxRxPlan plan;
    plan.slot = slot;
    plan.iq = iq;
    plan.prev_iq = iq;
    return plan;
}

// The main loop's step, every pass. Once the slot after `now_ms + kQmxRxLeadMs` is a new one, decides
// its path and returns true if the path changed. `busy`: the radio is transmitting, tuning, sweeping or
// priming, so a change is not made (the slot keeps the path it has and the plan moves on).
static inline bool qmx_rx_plan_step(QmxRxPlan* plan, int64_t now_ms, int64_t slot_ms, bool qmx_iq,
                                    int ab_slots, bool busy)
{
    const int64_t target = qmx_rx_slot_of(now_ms + kQmxRxLeadMs, slot_ms);
    if (target < plan->slot - 1) {  // the clock was set back: start over from here
        *plan = qmx_rx_plan_make(target, plan->iq);
        return false;
    }
    if (target <= plan->slot) {
        return false;
    }
    const bool want = qmx_rx_slot_wants_iq(target, qmx_iq, ab_slots);
    plan->prev_iq = plan->iq;
    plan->slot = target;
    if (want != plan->iq && !busy) {
        plan->iq = want;
        return true;
    }
    return false;
}

// The change just made did not reach the radio (the write failed, or it would land after the boundary):
// the capture stays with the radio, on the path it had.
static inline void qmx_rx_plan_cancel(QmxRxPlan* plan)
{
    plan->iq = plan->prev_iq;
}

// True once the plan's slot has begun: a Q9 sent now would land inside it.
static inline bool qmx_rx_plan_late(const QmxRxPlan& plan, int64_t now_ms, int64_t slot_ms)
{
    return now_ms >= plan.slot * slot_ms;
}

// The path at time `ms`, for the ingest task: the plan's, once its slot has begun. `applied` is the
// ingest task's own latch (start it at -1): once it has applied a plan's slot it keeps the plan's path,
// so a clock stepped back inside the plan's slot does not flip the capture while the radio stays switched.
static inline bool qmx_rx_plan_iq_at(const QmxRxPlan& plan, int64_t ms, int64_t slot_ms, int64_t* applied)
{
    if (qmx_rx_slot_of(ms, slot_ms) >= plan.slot) {
        *applied = plan.slot;
        return plan.iq;
    }
    return *applied == plan.slot ? plan.iq : plan.prev_iq;
}

// The path slot `slot` was received on, for its SLOT line. Right while the plan is at most one slot
// ahead of it, which a decode (it ends before its slot does) never exceeds.
static inline bool qmx_rx_plan_iq_for_slot(const QmxRxPlan& plan, int64_t slot)
{
    return slot >= plan.slot ? plan.iq : plan.prev_iq;
}

// One word, so the ingest task reads a whole plan at once. The slot is kept to 32 bits
// (a slot is 15 s: that lasts to the year 2100).
static inline uint64_t qmx_rx_plan_pack(const QmxRxPlan& plan)
{
    return (uint64_t)(uint32_t)plan.slot | ((uint64_t)plan.iq << 32) | ((uint64_t)plan.prev_iq << 33);
}

static inline QmxRxPlan qmx_rx_plan_unpack(uint64_t word)
{
    QmxRxPlan plan;
    plan.slot = (int64_t)(int32_t)(uint32_t)word;
    plan.iq = ((word >> 32) & 1) != 0;
    plan.prev_iq = ((word >> 33) & 1) != 0;
    return plan;
}
