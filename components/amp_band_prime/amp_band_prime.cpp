#include "amp_band_prime.h"

#include <atomic>

static constexpr int64_t kStallMs = 1000;

enum class Phase {
    Idle,
    Key,
    Wait,
    Unkey,
    Digital,
};

namespace {

struct Prime {
    const radio_swr_io_t* io;
    amp_band_prime_event_fn on_event;
    void* user;
    Phase phase;
    int64_t next_at;
    int64_t last_now;
    bool aborted;
    amp_band_prime_abort_reason_t abort_reason;
};

struct Pending {
    const radio_swr_io_t* io;
    amp_band_prime_event_fn on_event;
    void* user;
};

Prime s = {};
Pending pending = {};
std::atomic<int> pending_state{0};  // 0 none, 1 being written, 2 ready
std::atomic<int> cancel_requested{0};
std::atomic<int> busy{0};

void emit_and_reset()
{
    if (s.on_event) {
        const amp_band_prime_event_t event = {
            s.aborted ? AMP_BAND_PRIME_ABORTED : AMP_BAND_PRIME_DONE,
            s.abort_reason,
        };
        s.on_event(&event, s.user);
    }
    s = {};
    s.phase = Phase::Idle;
    cancel_requested.store(0);
    busy.store(0);
}

void begin_abort(amp_band_prime_abort_reason_t reason)
{
    s.aborted = true;
    s.abort_reason = reason;
    s.phase = Phase::Unkey;
}

void note_cat_failure()
{
    s.aborted = true;
    s.abort_reason = AMP_BAND_PRIME_ABORT_CAT_LOST;
}

}  // namespace

amp_band_prime_start_result_t amp_band_prime_request_start(
    const radio_swr_io_t* io, amp_band_prime_event_fn on_event, void* user)
{
    if (!io || !io->key || !io->unkey || !io->mode_digital) {
        return AMP_BAND_PRIME_START_UNSUPPORTED;
    }
    int expected = 0;
    if (!busy.compare_exchange_strong(expected, 1)) {
        return AMP_BAND_PRIME_START_BUSY;
    }
    expected = 0;
    if (!pending_state.compare_exchange_strong(expected, 1)) {
        busy.store(0);
        return AMP_BAND_PRIME_START_BUSY;
    }
    pending = {io, on_event, user};
    cancel_requested.store(0);
    pending_state.store(2);
    return AMP_BAND_PRIME_START_OK;
}

void amp_band_prime_request_cancel(void)
{
    if (busy.load()) {
        cancel_requested.store(1);
    }
}

int amp_band_prime_busy(void)
{
    return busy.load();
}

int amp_band_prime_active(void)
{
    return s.phase != Phase::Idle;
}

void amp_band_prime_tick(int64_t now_ms)
{
    if (s.phase == Phase::Idle) {
        if (pending_state.load() != 2) {
            return;
        }
        s.io = pending.io;
        s.on_event = pending.on_event;
        s.user = pending.user;
        s.phase = Phase::Key;
        s.last_now = now_ms;
        pending_state.store(0);
        if (cancel_requested.load()) {
            s.aborted = true;
            s.abort_reason = AMP_BAND_PRIME_ABORT_CANCELLED;
            emit_and_reset();
            return;
        }
    }

    if (cancel_requested.load()) {
        if (s.phase == Phase::Key) {
            s.aborted = true;
            s.abort_reason = AMP_BAND_PRIME_ABORT_CANCELLED;
            emit_and_reset();
            return;
        }
        if (s.phase == Phase::Wait) {
            begin_abort(AMP_BAND_PRIME_ABORT_CANCELLED);
        }
    }

    if (s.phase == Phase::Wait && now_ms - s.last_now > kStallMs) {
        begin_abort(AMP_BAND_PRIME_ABORT_STALLED);
    }
    s.last_now = now_ms;

    switch (s.phase) {
        case Phase::Idle:
            return;
        case Phase::Key:
            if (s.io->key() != ESP_OK) {
                note_cat_failure();
                s.phase = Phase::Unkey;
                return;
            }
            s.next_at = now_ms + AMP_BAND_PRIME_PULSE_MS;
            s.phase = Phase::Wait;
            return;
        case Phase::Wait:
            if (now_ms < s.next_at) {
                return;
            }
            s.phase = Phase::Unkey;
            [[fallthrough]];
        case Phase::Unkey:
            if (s.io->unkey() != ESP_OK) {
                note_cat_failure();
            }
            s.phase = Phase::Digital;
            return;
        case Phase::Digital:
            if (s.io->mode_digital() != ESP_OK) {
                note_cat_failure();
            }
            emit_and_reset();
            return;
    }
}
