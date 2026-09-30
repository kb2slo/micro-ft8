#include "swr_sweep.h"

#include <atomic>
#include <string.h>

// Waits, from the bench (docs/architecture/radio.md): the QMX answers a query within a few
// milliseconds but settles slowly after a setter, and the first SW; after keying can spike.
static constexpr int64_t kSetterWaitMs = 100;
static constexpr int64_t kDiscardMs = 300;
static constexpr int64_t kReplyTimeoutMs = 300;
static constexpr int64_t kReadGapMs = 50;
static constexpr int kReadsPerPoint = 3;
static constexpr int kMaxSilentInARow = 3;
static constexpr uint32_t kMinKhz = 100;
static constexpr uint32_t kMaxKhz = 60000;
static constexpr int64_t kStallMs = 1000;

enum class Phase {
    Idle,
    TuneUnkey,     // RX;
    TuneMode,      // MD6;
    TuneSet,       // FA<hz>;
    TuneQuery,     // FA; and wait for the answer
    Key,           // MD8;
    Read,          // SW; and wait, up to kReadsPerPoint times
    Verify,        // FA; while keyed: did it hold
    FinishUnkey,   // RX;
    FinishRestore, // back to the operating frequency, then the final event
};

enum class Purpose {
    ProbeLow,   // the first frequency of a range, tried to see whether the radio has the band
    ProbeHigh,  // the last, tried when the first was refused
    Point,
};

enum class TuneResult { Held, Refused, Silent };

namespace {

struct Sweep {
    swr_sweep_range_t ranges[SWR_SWEEP_MAX_RANGES];
    size_t range_count;
    size_t range_index;
    uint32_t point_index;
    uint32_t restore_hz;
    const radio_swr_io_t* io;
    swr_sweep_event_fn on_event;
    void* user;

    Phase phase;
    Purpose purpose;
    uint32_t target_hz;
    int64_t next_at;
    int64_t last_now;
    int64_t deadline;
    bool query_sent;
    int silent_in_a_row;
    int reads[kReadsPerPoint];
    int read_count;
    int read_attempts;
    bool finish_aborted;
    swr_sweep_abort_reason_t finish_reason;
};

Sweep s;

// A request from another task, handed to the ticking one. `pending_state`: 0 none, 1 being
// written, 2 ready. `busy` covers the wait and the run.
struct Pending {
    swr_sweep_range_t ranges[SWR_SWEEP_MAX_RANGES];
    size_t count;
    uint32_t restore_hz;
    const radio_swr_io_t* io;
    swr_sweep_event_fn on_event;
    void* user;
};
Pending pending;
std::atomic<int> pending_state{0};
std::atomic<int> cancel_requested{0};
std::atomic<int> busy{0};

void emit(swr_sweep_event_kind_t kind, uint32_t freq_hz, int swr, swr_sweep_abort_reason_t reason)
{
    if (!s.on_event) {
        return;
    }
    swr_sweep_event_t event = {};
    event.kind = kind;
    event.range_index = (uint8_t)s.range_index;
    event.freq_hz = freq_hz;
    event.swr_hundredths = swr;
    event.abort_reason = reason;
    s.on_event(&event, s.user);
}

uint32_t range_points(const swr_sweep_range_t& range)
{
    const uint32_t span = range.hi_khz - range.lo_khz;
    return span / range.step_khz + 1 + ((span % range.step_khz) ? 1 : 0);
}

uint32_t point_hz(const swr_sweep_range_t& range, uint32_t index)
{
    uint64_t khz = (uint64_t)range.lo_khz + (uint64_t)index * range.step_khz;
    if (khz > range.hi_khz) {
        khz = range.hi_khz;
    }
    return (uint32_t)khz * 1000u;
}

void begin_finish(bool aborted, swr_sweep_abort_reason_t reason)
{
    s.finish_aborted = aborted;
    s.finish_reason = reason;
    s.phase = Phase::FinishUnkey;
    s.next_at = s.last_now;
}

void begin_tune(Purpose purpose, uint32_t hz)
{
    s.purpose = purpose;
    s.target_hz = hz;
    s.query_sent = false;
    s.phase = Phase::TuneUnkey;
    s.next_at = s.last_now;
}

void begin_range()
{
    s.point_index = 0;
    begin_tune(Purpose::ProbeLow, point_hz(s.ranges[s.range_index], 0));
}

// The range is finished (measured or skipped): the next one, or the end.
void next_range()
{
    ++s.range_index;
    if (s.range_index >= s.range_count) {
        begin_finish(false, SWR_SWEEP_ABORT_CANCELLED);
        return;
    }
    begin_range();
}

// The point at s.point_index is finished (measured or skipped): the next one.
void next_point()
{
    const swr_sweep_range_t& range = s.ranges[s.range_index];
    ++s.point_index;
    if (s.point_index >= range_points(range)) {
        next_range();
        return;
    }
    begin_tune(Purpose::Point, point_hz(range, s.point_index));
}

// A query that got no answer. Three in a row mean the radio is not there.
bool note_silent()
{
    if (++s.silent_in_a_row >= kMaxSilentInARow) {
        begin_finish(true, SWR_SWEEP_ABORT_CAT_LOST);
        return false;
    }
    return true;
}

void lost_cat()
{
    begin_finish(true, SWR_SWEEP_ABORT_CAT_LOST);
}

void after_tune(TuneResult result)
{
    switch (s.purpose) {
        case Purpose::ProbeLow:
            if (result == TuneResult::Held) {
                // The first frequency is tuned already: measure it without tuning again.
                s.purpose = Purpose::Point;
                s.phase = Phase::Key;
                s.next_at = s.last_now;
            } else {
                begin_tune(Purpose::ProbeHigh, point_hz(s.ranges[s.range_index], range_points(s.ranges[s.range_index]) - 1));
            }
            return;
        case Purpose::ProbeHigh:
            if (result == TuneResult::Refused) {
                // Both edges answered "no": the radio does not have this band.
                emit(SWR_SWEEP_EVENT_BAND_SKIPPED, 0, -1, SWR_SWEEP_ABORT_CANCELLED);
                next_range();
            } else {
                // The radio has the band but not its first frequency (skip that point only), or
                // did not answer (try the points; a silent radio is caught by the silent count).
                next_point();
            }
            return;
        case Purpose::Point:
            if (result == TuneResult::Held) {
                s.phase = Phase::Key;
                s.next_at = s.last_now;
            } else {
                next_point();
            }
            return;
    }
}

int median_of(int* values, int count)
{
    for (int i = 1; i < count; ++i) {
        const int value = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > value) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = value;
    }
    return values[count / 2];
}

}  // namespace

uint32_t swr_sweep_point_count(const swr_sweep_range_t* ranges, size_t count)
{
    if (!ranges || count == 0 || count > SWR_SWEEP_MAX_RANGES) {
        return 0;
    }
    uint32_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const swr_sweep_range_t& range = ranges[i];
        if (range.step_khz == 0 || range.lo_khz < kMinKhz || range.hi_khz > kMaxKhz || range.lo_khz > range.hi_khz) {
            return 0;
        }
        total += range_points(range);
        if (total > SWR_SWEEP_MAX_POINTS) {
            return total;
        }
    }
    return total;
}

uint32_t swr_sweep_estimate_ms(const swr_sweep_range_t* ranges, size_t count)
{
    return swr_sweep_point_count(ranges, count) * SWR_SWEEP_POINT_MS;
}

static swr_sweep_start_result_t validate(const swr_sweep_range_t* ranges, size_t count, uint32_t restore_hz,
                                         const radio_swr_io_t* io)
{
    if (!io) {
        return SWR_SWEEP_START_UNSUPPORTED;
    }
    if (count > SWR_SWEEP_MAX_RANGES) {
        return SWR_SWEEP_START_TOO_MANY;
    }
    const uint32_t points = swr_sweep_point_count(ranges, count);
    if (points == 0 || restore_hz == 0) {
        return SWR_SWEEP_START_BAD_RANGE;
    }
    if (points > SWR_SWEEP_MAX_POINTS) {
        return SWR_SWEEP_START_TOO_MANY;
    }
    return SWR_SWEEP_START_OK;
}

swr_sweep_start_result_t swr_sweep_start(const swr_sweep_range_t* ranges, size_t count, uint32_t restore_hz,
                                         const radio_swr_io_t* io, swr_sweep_event_fn on_event, void* user,
                                         int64_t now_ms)
{
    const swr_sweep_start_result_t checked = validate(ranges, count, restore_hz, io);
    if (checked != SWR_SWEEP_START_OK) {
        return checked;
    }
    if (s.phase != Phase::Idle) {
        return SWR_SWEEP_START_BUSY;
    }
    memset(&s, 0, sizeof(s));
    memcpy(s.ranges, ranges, count * sizeof(swr_sweep_range_t));
    s.range_count = count;
    s.restore_hz = restore_hz;
    s.io = io;
    s.on_event = on_event;
    s.user = user;
    s.last_now = now_ms;
    busy.store(1);
    begin_range();
    return SWR_SWEEP_START_OK;
}

swr_sweep_start_result_t swr_sweep_request_start(const swr_sweep_range_t* ranges, size_t count,
                                                 uint32_t restore_hz, const radio_swr_io_t* io,
                                                 swr_sweep_event_fn on_event, void* user)
{
    const swr_sweep_start_result_t checked = validate(ranges, count, restore_hz, io);
    if (checked != SWR_SWEEP_START_OK) {
        return checked;
    }
    int expected = 0;
    if (!busy.compare_exchange_strong(expected, 1)) {
        return SWR_SWEEP_START_BUSY;
    }
    memcpy(pending.ranges, ranges, count * sizeof(swr_sweep_range_t));
    pending.count = count;
    pending.restore_hz = restore_hz;
    pending.io = io;
    pending.on_event = on_event;
    pending.user = user;
    pending_state.store(2);
    return SWR_SWEEP_START_OK;
}

void swr_sweep_request_cancel(void)
{
    cancel_requested.store(1);
}

int swr_sweep_busy(void)
{
    return busy.load();
}

void swr_sweep_cancel(void)
{
    if (s.phase == Phase::Idle || s.phase == Phase::FinishUnkey || s.phase == Phase::FinishRestore) {
        return;
    }
    // De-key first: the carrier must not wait for the next tick.
    s.io->unkey();
    begin_finish(true, SWR_SWEEP_ABORT_CANCELLED);
}

int swr_sweep_active(void)
{
    return s.phase != Phase::Idle;
}

static void report_unstarted(swr_sweep_abort_reason_t reason)
{
    if (pending.on_event) {
        swr_sweep_event_t event = {};
        event.kind = SWR_SWEEP_EVENT_ABORTED;
        event.swr_hundredths = -1;
        event.abort_reason = reason;
        pending.on_event(&event, pending.user);
    }
}

// Work another task asked for: a cancel, then a start. Runs on the ticking task.
static void take_requests(int64_t now_ms)
{
    if (cancel_requested.exchange(0)) {
        if (s.phase != Phase::Idle) {
            swr_sweep_cancel();
        } else if (pending_state.load() == 2) {
            pending_state.store(0);
            report_unstarted(SWR_SWEEP_ABORT_CANCELLED);
            busy.store(0);
        }
    }
    if (s.phase == Phase::Idle && pending_state.load() == 2) {
        const swr_sweep_start_result_t result = swr_sweep_start(pending.ranges, pending.count, pending.restore_hz,
                                                                pending.io, pending.on_event, pending.user, now_ms);
        pending_state.store(0);
        if (result != SWR_SWEEP_START_OK) {
            report_unstarted(SWR_SWEEP_ABORT_CAT_LOST);
            busy.store(0);
        }
    }
}

void swr_sweep_tick(int64_t now_ms)
{
    take_requests(now_ms);
    if (s.phase == Phase::Idle) {
        return;
    }
    // The radio is keyed in Read and Verify. A tick that comes late there means whatever runs the
    // tick was stuck; do not leave the carrier up until the next command.
    if ((s.phase == Phase::Read || s.phase == Phase::Verify) && now_ms - s.last_now > kStallMs) {
        s.io->unkey();
        s.last_now = now_ms;
        begin_finish(true, SWR_SWEEP_ABORT_STALLED);
        return;
    }
    s.last_now = now_ms;
    if (now_ms < s.next_at) {
        return;
    }
    const radio_swr_io_t* io = s.io;

    switch (s.phase) {
        case Phase::Idle:
            return;

        case Phase::TuneUnkey:
            if (io->unkey() != ESP_OK) {
                lost_cat();
                return;
            }
            s.phase = Phase::TuneMode;
            s.next_at = now_ms + kSetterWaitMs;
            return;

        case Phase::TuneMode:
            if (io->mode_digital() != ESP_OK) {
                lost_cat();
                return;
            }
            s.phase = Phase::TuneSet;
            s.next_at = now_ms + kSetterWaitMs;
            return;

        case Phase::TuneSet:
            if (io->set_frequency((int)s.target_hz) != ESP_OK) {
                lost_cat();
                return;
            }
            s.phase = Phase::TuneQuery;
            s.query_sent = false;
            s.next_at = now_ms + kSetterWaitMs;
            return;

        case Phase::TuneQuery: {
            if (!s.query_sent) {
                io->clear_replies();
                if (io->query_frequency() != ESP_OK) {
                    lost_cat();
                    return;
                }
                s.query_sent = true;
                s.deadline = now_ms + kReplyTimeoutMs;
                return;
            }
            int hz = -1;
            if (io->frequency_reply(&hz)) {
                s.silent_in_a_row = 0;
                after_tune(hz == (int)s.target_hz ? TuneResult::Held : TuneResult::Refused);
            } else if (now_ms >= s.deadline) {
                if (note_silent()) {
                    after_tune(TuneResult::Silent);
                }
            }
            return;
        }

        case Phase::Key:
            if (io->key() != ESP_OK) {
                lost_cat();
                return;
            }
            s.read_count = 0;
            s.read_attempts = 0;
            s.query_sent = false;
            s.phase = Phase::Read;
            s.next_at = now_ms + kDiscardMs;
            return;

        case Phase::Read: {
            if (!s.query_sent) {
                io->clear_replies();
                if (io->query_swr() != ESP_OK) {
                    lost_cat();
                    return;
                }
                s.query_sent = true;
                s.deadline = now_ms + kReplyTimeoutMs;
                return;
            }
            int swr = -1;
            bool done_with_read = false;
            if (io->swr_reply(&swr)) {
                s.silent_in_a_row = 0;
                s.reads[s.read_count++] = swr;
                done_with_read = true;
            } else if (now_ms >= s.deadline) {
                if (!note_silent()) {
                    return;
                }
                done_with_read = true;
            }
            if (!done_with_read) {
                return;
            }
            s.query_sent = false;
            if (++s.read_attempts < kReadsPerPoint) {
                s.next_at = now_ms + kReadGapMs;
            } else {
                s.phase = Phase::Verify;
                s.next_at = now_ms;
            }
            return;
        }

        case Phase::Verify: {
            if (!s.query_sent) {
                io->clear_replies();
                if (io->query_frequency() != ESP_OK) {
                    lost_cat();
                    return;
                }
                s.query_sent = true;
                s.deadline = now_ms + kReplyTimeoutMs;
                return;
            }
            int hz = -1;
            bool answered = io->frequency_reply(&hz);
            if (!answered && now_ms < s.deadline) {
                return;
            }
            if (!answered && !note_silent()) {
                return;
            }
            if (answered) {
                s.silent_in_a_row = 0;
            }
            // A point counts only when the radio still reported the requested frequency while
            // keyed and at least one SW; answered.
            if (answered && hz == (int)s.target_hz && s.read_count > 0) {
                emit(SWR_SWEEP_EVENT_POINT, s.target_hz, median_of(s.reads, s.read_count),
                     SWR_SWEEP_ABORT_CANCELLED);
            }
            next_point();
            return;
        }

        case Phase::FinishUnkey:
            io->unkey();
            s.phase = Phase::FinishRestore;
            s.next_at = now_ms + kSetterWaitMs;
            return;

        case Phase::FinishRestore:
            io->restore((int)s.restore_hz);
            s.phase = Phase::Idle;
            if (s.finish_aborted) {
                emit(SWR_SWEEP_EVENT_ABORTED, 0, -1, s.finish_reason);
            } else {
                emit(SWR_SWEEP_EVENT_DONE, 0, -1, SWR_SWEEP_ABORT_CANCELLED);
            }
            busy.store(0);
            return;
    }
}
