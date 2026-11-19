// swr_trip: what the main board does when the QMX's SWR protection stops a transmission
// (components/swr_trip/include/swr_trip.h).

#include <cstdio>

#include "swr_trip.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

int main()
{
    // A band change trips once, then transmits: retried, and the next clean one resets the count.
    SwrTrip t;
    expect(swr_trip_on_trip(t) == SwrTripAction::Retry, "the first trip is retried");
    expect(!t.stopped, "one trip does not stop the station");
    swr_trip_on_clean(t);
    expect(swr_trip_on_trip(t) == SwrTripAction::Retry,
           "a trip after a clean transmission is the first again");
    expect(!t.stopped, "and still does not stop it");

    // An antenna fault trips every time: the second in a row stops.
    expect(swr_trip_on_trip(t) == SwrTripAction::Stop, "a second trip in a row stops");
    expect(t.stopped, "and the station stays stopped");

    // Starting something clears it, and says the radio's lock needs clearing.
    expect(swr_trip_operator_start(t), "starting from the stopped state asks for the lock to be cleared");
    expect(!t.stopped && t.trips_in_row == 0, "and the station is running with the count at zero");
    expect(!swr_trip_operator_start(t), "starting when not stopped clears nothing");
    expect(swr_trip_on_trip(t) == SwrTripAction::Retry, "after a restart the next trip is retried");

    // Locked before we keyed: stopped at once, and a start clears it.
    SwrTrip locked;
    swr_trip_on_locked_before_tx(locked);
    expect(locked.stopped, "a lock set before we keyed starts the station stopped");
    expect(swr_trip_operator_start(locked), "and the operator's start clears the lock");
    expect(swr_trip_on_trip(locked) == SwrTripAction::Retry, "and the count starts again");

    // Tune: stopped above the limit, never on no reading.
    expect(!swr_tune_over(-1.f), "no reading yet does not stop Tune");
    expect(!swr_tune_over(1.4f), "a matched antenna does not stop Tune");
    expect(!swr_tune_over(kSwrTuneLimit), "SWR at the limit does not stop Tune");
    expect(swr_tune_over(3.01f), "SWR over the limit stops Tune");
    expect(swr_tune_over(9.98f), "the antenna off (SWR 9.98, 2026-11-02) stops Tune");
    expect(kSwrTuneLimit == 3.0f, "the limit is the QMX's default SWR threshold");

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("swr_trip: ok\n");
    return 0;
}
