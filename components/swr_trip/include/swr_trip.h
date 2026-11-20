#pragma once

// What the main board does when the QMX's SWR protection stops a transmission
// (docs/architecture/radio.md, SWR protection trips on a QMX). A band change through an
// amplifier trips it once and then transmits; an antenna fault trips every time, and the
// count tells the two apart. Header only so host_mock can test it; the radio and the
// queue are poe_p4/main/tx_scheduler.cpp's.

constexpr int kSwrTripLimit = 2;  // tripped transmissions in a row that stop the station

struct SwrTrip {
    int trips_in_row = 0;
    bool stopped = false;  // slots are skipped until the operator starts something
};

enum class SwrTripAction {
    Retry,  // clear the lock; the entry goes out in its next slot
    Stop,   // leave the lock set, end the CQ and cancel every QSO
};

// A transmission was stopped by the lock.
inline SwrTripAction swr_trip_on_trip(SwrTrip& t) {
    ++t.trips_in_row;
    if (t.trips_in_row < kSwrTripLimit) {
        return SwrTripAction::Retry;
    }
    t.stopped = true;
    return SwrTripAction::Stop;
}

// A transmission went out whole.
inline void swr_trip_on_clean(SwrTrip& t) {
    t.trips_in_row = 0;
}

// The lock was already set when the radio connected, so nothing can go out.
inline void swr_trip_on_locked_before_tx(SwrTrip& t) {
    t.trips_in_row = kSwrTripLimit;
    t.stopped = true;
}

// The operator called CQ, replied or pressed Tune. True when the station was stopped,
// which is when the caller must clear the radio's lock.
inline bool swr_trip_operator_start(SwrTrip& t) {
    const bool was_stopped = t.stopped;
    t.trips_in_row = 0;
    t.stopped = false;
    return was_stopped;
}

// Tune is stopped by the P4 above this SWR (docs/architecture/radio.md, SWR protection trips on a QMX): the
// QMX's own Protection > SWR threshold at its default. The radio's protection did not stop Tune at SWR 9.98 on
// 1_04_010 (2026-11-02), so the P4 reads the SWR Tune reports and ends it itself.
constexpr float kSwrTuneLimit = 3.0f;

// A Tune SWR reading (`swr` < 0: none yet): true when Tune must end. One reading is enough: SWR Tune's readings
// stray by 0.3 at most (radio.md, QMX SWR Tune over CAT), and a carrier into a bad antenna should not run on.
inline bool swr_tune_over(float swr) {
    return swr > kSwrTuneLimit;
}
