// qmx_rx_mode: which receive path each slot uses, and when a change lands
// (components/radio_control/include/qmx_rx_mode.h).

#include <cstdio>
#include <string>

#include "qmx_rx_mode.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static const int64_t kSlot = 15000;

// The schedule as a string of slots 0..n-1, i for IQ and a for audio.
static std::string schedule(bool qmx_iq, int ab, int n)
{
    std::string out;
    for (int s = 0; s < n; ++s) out += qmx_rx_slot_wants_iq(s, qmx_iq, ab) ? 'i' : 'a';
    return out;
}

// Runs the main loop's step every 10 ms over [from_ms, to_ms), recording the first time the plan
// reported a change and the path the ingest task would have seen at each step.
struct Run {
    QmxRxPlan plan;
    int64_t changed_at = -1;
    int changes = 0;
};

static void drive(Run* run, int64_t from_ms, int64_t to_ms, bool qmx_iq, int ab, bool busy = false)
{
    for (int64_t t = from_ms; t < to_ms; t += 10) {
        if (qmx_rx_plan_step(&run->plan, t, kSlot, qmx_iq, ab, busy)) {
            if (run->changed_at < 0) run->changed_at = t;
            ++run->changes;
        }
    }
}

int main()
{
    // The A/B schedule: N slots of IQ, N of audio, from slot 0, whatever qmx_iq says.
    expect(schedule(false, 2, 8) == "iiaaiiaa", "N = 2 alternates in pairs");
    expect(schedule(true, 2, 8) == "iiaaiiaa", "and qmx_iq does not change it");
    expect(schedule(false, 1, 6) == "iaiaia", "N = 1 alternates every slot");
    expect(schedule(false, 3, 12) == "iiiaaaiiiaaa", "N = 3");
    expect(schedule(false, 0, 4) == "aaaa" && schedule(true, 0, 4) == "iiii", "N = 0 follows qmx_iq");
    expect(schedule(false, -1, 2) == "aa", "a negative N is off");
    {
        // Each path gets both parities of the 15 s cycle at N = 2, which N = 1 does not give a path.
        bool even_iq = false, odd_iq = false, even_au = false, odd_au = false;
        for (int s = 0; s < 8; ++s) {
            const bool iq = qmx_rx_slot_wants_iq(s, false, 2);
            (iq ? (s % 2 ? odd_iq : even_iq) : (s % 2 ? odd_au : even_au)) = true;
        }
        expect(even_iq && odd_iq && even_au && odd_au, "N = 2 gives each path an even and an odd slot");
    }

    // The change lands kQmxRxLeadMs before the boundary: slot 4 begins at 60,000 ms.
    {
        Run run;
        run.plan = qmx_rx_plan_make(0, true);
        drive(&run, 0, 70000, false, 2);  // slots 0,1 IQ; 2,3 audio; 4,5 IQ
        expect(run.changes == 2, "two changes in slots 0..4: to audio at 2, to IQ at 4");
        QmxRxPlan p = qmx_rx_plan_make(0, true);
        int64_t first = -1;
        for (int64_t t = 0; t < 30000; t += 10) {
            if (qmx_rx_plan_step(&p, t, kSlot, false, 2, false) && first < 0) first = t;
        }
        expect(first >= 2 * kSlot - kQmxRxLeadMs && first < 2 * kSlot - kQmxRxLeadMs + 10,
               "the change to audio is made 0.5 s before slot 2");
    }

    // The ingest task's view: the path is constant within each slot, and changes at the boundary.
    {
        Run run;
        run.plan = qmx_rx_plan_make(0, true);
        bool ok = true, at_boundary = true;
        bool last = true;
        for (int64_t t = 0; t < 8 * kSlot; t += 10) {
            qmx_rx_plan_step(&run.plan, t, kSlot, false, 2, false);
            const bool seen = qmx_rx_plan_iq_at(run.plan, t, kSlot);
            const int64_t slot = t / kSlot;
            if (seen != qmx_rx_slot_wants_iq(slot, false, 2)) ok = false;
            if (seen != last && t % kSlot != 0) at_boundary = false;
            last = seen;
        }
        expect(ok, "the path in effect at any time is the schedule's for that slot");
        expect(at_boundary, "and it changes only at a slot's first step");
    }

    // A slot's own path, for its SLOT line, when its decode ends after the next change was decided.
    {
        QmxRxPlan p = qmx_rx_plan_make(0, true);
        qmx_rx_plan_step(&p, 2 * kSlot - 400, kSlot, false, 2, false);  // slot 2 decided: audio
        qmx_rx_plan_step(&p, 3 * kSlot - 400, kSlot, false, 2, false);  // slot 3: audio
        expect(p.slot == 3 && !p.iq && !p.prev_iq, "slot 3 audio, slot 2 audio");
        qmx_rx_plan_step(&p, 4 * kSlot - 400, kSlot, false, 2, false);  // slot 4: IQ
        expect(p.slot == 4 && p.iq && !p.prev_iq, "slot 4 IQ after audio");
        expect(!qmx_rx_plan_iq_for_slot(p, 3) && qmx_rx_plan_iq_for_slot(p, 4) && qmx_rx_plan_iq_for_slot(p, 5),
               "slot 3 reads audio while its decode finishes in slot 3's last 0.5 s");
    }

    // qmx_iq changed at run time, mid-slot, takes effect at the next boundary and not before.
    {
        Run run;
        run.plan = qmx_rx_plan_make(3, false);
        drive(&run, 3 * kSlot, 3 * kSlot + 7000, false, 0);
        drive(&run, 3 * kSlot + 7000, 4 * kSlot - 600, true, 0);
        expect(run.changes == 0 && !qmx_rx_plan_iq_at(run.plan, 4 * kSlot - 600, kSlot), "key set mid-slot: nothing yet");
        drive(&run, 4 * kSlot - 600, 4 * kSlot - 400, true, 0);
        expect(run.changes == 1 && run.changed_at >= 4 * kSlot - 500 && run.changed_at < 4 * kSlot - 490,
               "decided 0.5 s before the boundary");
        expect(!qmx_rx_plan_iq_at(run.plan, 4 * kSlot - 1, kSlot) && qmx_rx_plan_iq_at(run.plan, 4 * kSlot, kSlot),
               "and in effect from the boundary");
        drive(&run, 4 * kSlot - 400, 9 * kSlot, true, 0);
        expect(run.changes == 1, "and not decided again");
    }

    // Busy at the decision: the slot keeps its path, the radio is not touched, the next slot asks again.
    {
        QmxRxPlan p = qmx_rx_plan_make(1, true);
        expect(!qmx_rx_plan_step(&p, 2 * kSlot - 400, kSlot, false, 0, true), "busy: no change reported");
        expect(p.iq && p.prev_iq && p.slot == 2, "busy: slot 2 stays IQ");
        expect(qmx_rx_plan_step(&p, 3 * kSlot - 400, kSlot, false, 0, false), "next slot, idle: the change is made");
        expect(!p.iq && p.prev_iq && p.slot == 3, "to audio from slot 3");
        // Busy and no change wanted: nothing to refuse.
        QmxRxPlan q = qmx_rx_plan_make(1, true);
        qmx_rx_plan_step(&q, 2 * kSlot - 400, kSlot, true, 0, true);
        expect(q.iq && q.slot == 2, "busy with the path unchanged still moves on");
    }

    // A clock set back restarts the plan; a small step back does nothing.
    {
        QmxRxPlan p = qmx_rx_plan_make(100, true);
        expect(!qmx_rx_plan_step(&p, 100 * kSlot - 600, kSlot, false, 0, false) && p.slot == 100,
               "a step back inside a slot is ignored");
        expect(!qmx_rx_plan_step(&p, 5 * kSlot, kSlot, false, 0, false) && p.slot == 5 && p.iq && p.prev_iq,
               "a clock set far back restarts the plan on the path it has");
    }

    // The plan is one word.
    {
        QmxRxPlan p;
        p.slot = 113000000;
        p.iq = true;
        p.prev_iq = false;
        QmxRxPlan q = qmx_rx_plan_unpack(qmx_rx_plan_pack(p));
        expect(q.slot == p.slot && q.iq && !q.prev_iq, "pack round-trips");
        p.iq = false;
        p.prev_iq = true;
        p.slot = 0;
        q = qmx_rx_plan_unpack(qmx_rx_plan_pack(p));
        expect(q.slot == 0 && !q.iq && q.prev_iq, "and with the flags the other way");
    }

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("qmx_rx_mode: ok\n");
    return 0;
}
