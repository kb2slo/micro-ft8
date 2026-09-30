// swr_sweep: the sweep sequence, against a fake QMX (components/swr_sweep/include/swr_sweep.h).
// The fake keeps the behaviour measured on the bench (docs/architecture/radio.md): it refuses
// FA outside its configured bands, keys itself on MD8, and reports the SWR in hundredths only
// while keyed.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "swr_sweep.h"

static int g_fails = 0;

static void check(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

struct Band {
    int lo_khz;
    int hi_khz;
};

struct Fake {
    std::vector<Band> bands;
    int fa = 21074000;
    int md = 6;
    bool keyed = false;
    bool ever_keyed = false;
    bool silent = false;
    bool fail_key = false;
    bool fa_drifts_when_keyed = false;  // the v2 bench bug: MD8 snapped FA back
    std::vector<int> swr_script;        // readings handed out first, then the default
    int fa_reply = -1;
    int sw_reply = -1;
    int restores = 0;
    int restore_hz = 0;
    std::vector<std::string> log;
};

static Fake g;

static int default_swr(int hz)
{
    return 100 + (hz / 1000) % 100;
}

static esp_err_t f_unkey() { g.log.push_back("RX"); g.keyed = false; return ESP_OK; }
static esp_err_t f_mode() { g.log.push_back("MD6"); g.md = 6; g.keyed = false; return ESP_OK; }
static esp_err_t f_set(int hz)
{
    g.log.push_back("FA" + std::to_string(hz));
    if (g.keyed) {
        return ESP_OK;  // refused while keyed
    }
    for (const Band& b : g.bands) {
        if (hz >= b.lo_khz * 1000 && hz <= b.hi_khz * 1000) {
            g.fa = hz;
        }
    }
    return ESP_OK;
}
static esp_err_t f_qfreq()
{
    g.log.push_back("FA?");
    if (!g.silent) {
        g.fa_reply = g.fa;
    }
    return ESP_OK;
}
static esp_err_t f_key()
{
    g.log.push_back("MD8");
    if (g.fail_key) {
        return ESP_FAIL;
    }
    g.md = 8;
    g.keyed = true;
    g.ever_keyed = true;
    if (g.fa_drifts_when_keyed) {
        g.fa = 21074000;
    }
    return ESP_OK;
}
static esp_err_t f_qswr()
{
    g.log.push_back("SW?");
    if (!g.silent && g.keyed) {
        if (!g.swr_script.empty()) {
            g.sw_reply = g.swr_script.front();
            g.swr_script.erase(g.swr_script.begin());
        } else {
            g.sw_reply = default_swr(g.fa);
        }
    }
    return ESP_OK;
}
static bool f_freply(int* hz)
{
    if (g.fa_reply < 0) {
        return false;
    }
    *hz = g.fa_reply;
    return true;
}
static bool f_sreply(int* value)
{
    if (g.sw_reply < 0) {
        return false;
    }
    *value = g.sw_reply;
    return true;
}
static void f_clear() { g.fa_reply = -1; g.sw_reply = -1; }
static esp_err_t f_restore(int hz)
{
    g.log.push_back("RESTORE");
    g.restores++;
    g.restore_hz = hz;
    g.md = 6;
    g.keyed = false;
    g.fa = hz;
    return ESP_OK;
}

static const radio_swr_io_t k_io = {f_unkey, f_mode, f_set, f_qfreq, f_key, f_qswr,
                                    f_freply, f_sreply, f_clear, f_restore};

struct Seen {
    std::vector<swr_sweep_event_t> events;
};

static void on_event(const swr_sweep_event_t* event, void* user)
{
    static_cast<Seen*>(user)->events.push_back(*event);
}

static int64_t g_now = 0;

static void reset(Fake fresh)
{
    g = fresh;
    g_now = 1000;
}

// Ticks every 10 ms until the sweep ends; false if it does not within the bound.
static bool run(int max_ticks = 200000)
{
    for (int i = 0; i < max_ticks && (swr_sweep_active() || swr_sweep_busy()); ++i) {
        g_now += 10;
        swr_sweep_tick(g_now);
    }
    return !swr_sweep_active() && !swr_sweep_busy();
}

static Fake qmx_with(std::vector<Band> bands)
{
    Fake f;
    f.bands = bands;
    return f;
}

static int count_kind(const Seen& seen, swr_sweep_event_kind_t kind)
{
    int n = 0;
    for (const auto& e : seen.events) {
        if (e.kind == kind) {
            ++n;
        }
    }
    return n;
}

static int count_cmd(const char* cmd)
{
    int n = 0;
    for (const auto& c : g.log) {
        if (c == cmd) {
            ++n;
        }
    }
    return n;
}

int main()
{
    // Plans: the point count, and what is refused.
    {
        const swr_sweep_range_t twenty[] = {{14000, 14350, 25}};
        check(swr_sweep_point_count(twenty, 1) == 15, "20 m at 25 kHz is 15 points");
        const swr_sweep_range_t forty[] = {{7000, 7300, 20}};
        check(swr_sweep_point_count(forty, 1) == 16, "40 m at 20 kHz is 16 points");
        const swr_sweep_range_t odd[] = {{100, 110, 4}};
        check(swr_sweep_point_count(odd, 1) == 4, "a range that does not divide evenly still ends on its top");
        const swr_sweep_range_t one[] = {{7100, 7100, 5}};
        check(swr_sweep_point_count(one, 1) == 1, "a single frequency is one point");
        const swr_sweep_range_t zero_step[] = {{7000, 7300, 0}};
        check(swr_sweep_point_count(zero_step, 1) == 0, "a zero step is invalid");
        const swr_sweep_range_t backwards[] = {{7300, 7000, 20}};
        check(swr_sweep_point_count(backwards, 1) == 0, "lo above hi is invalid");
        const swr_sweep_range_t too_low[] = {{50, 100, 10}};
        check(swr_sweep_point_count(too_low, 1) == 0, "below the radio's range is invalid");
        const swr_sweep_range_t too_high[] = {{50000, 70000, 100}};
        check(swr_sweep_point_count(too_high, 1) == 0, "above the radio's range is invalid");
        check(swr_sweep_point_count(nullptr, 0) == 0, "no ranges is invalid");
        check(swr_sweep_estimate_ms(twenty, 1) == 15u * SWR_SWEEP_POINT_MS, "the estimate is points times the per-point cost");

        swr_sweep_range_t many[SWR_SWEEP_MAX_RANGES + 1];
        for (auto& r : many) {
            r = {7000, 7010, 10};
        }
        check(swr_sweep_point_count(many, SWR_SWEEP_MAX_RANGES + 1) == 0, "more ranges than the cap is invalid");
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        check(swr_sweep_start(many, SWR_SWEEP_MAX_RANGES + 1, 14074000, &k_io, on_event, &seen, g_now) ==
                  SWR_SWEEP_START_TOO_MANY, "too many ranges is refused");
        const swr_sweep_range_t huge[] = {{1000, 59000, 1}};
        check(swr_sweep_start(huge, 1, 14074000, &k_io, on_event, &seen, g_now) == SWR_SWEEP_START_TOO_MANY,
              "more points than the cap is refused");
        check(swr_sweep_start(twenty, 1, 14074000, nullptr, on_event, &seen, g_now) == SWR_SWEEP_START_UNSUPPORTED,
              "a radio that cannot sweep is refused");
        check(swr_sweep_start(twenty, 1, 0, &k_io, on_event, &seen, g_now) == SWR_SWEEP_START_BAD_RANGE,
              "no frequency to restore to is refused");
        check(!swr_sweep_active() && g.log.empty(), "a refused start sends nothing");
    }

    // A full sweep of one configured range: the sequence, the points, the restore.
    {
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{7000, 7060, 20}};
        check(swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now) == SWR_SWEEP_START_OK, "start ok");
        check(swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now) == SWR_SWEEP_START_BUSY,
              "a second start while one runs is refused");
        check(run(), "the sweep ends");
        check(count_kind(seen, SWR_SWEEP_EVENT_POINT) == 4, "four points: 7000, 7020, 7040, 7060");
        check(seen.events.size() == 5 && seen.events.back().kind == SWR_SWEEP_EVENT_DONE, "a DONE event closes it");
        check(seen.events[0].freq_hz == 7000000 && seen.events[0].swr_hundredths == 100, "first point reads 1.00");
        check(seen.events[3].freq_hz == 7060000, "the last point is the top of the range");
        const std::vector<std::string> head = {"RX", "MD6", "FA7000000", "FA?", "MD8", "SW?", "SW?", "SW?", "FA?",
                                               "RX", "MD6", "FA7020000"};
        check(g.log.size() > head.size() && std::equal(head.begin(), head.end(), g.log.begin()),
              "per point: RX, MD6, FA, verify, MD8, three SW, verify; the first point is not tuned twice");
        check(!g.keyed && g.restores == 1 && g.restore_hz == 14074000 && g.md == 6 && g.fa == 14074000,
              "afterwards the radio is unkeyed, in MD6, back on the operating frequency");
        check(g.log.back() == "RESTORE" && g.log[g.log.size() - 2] == "RX", "the last commands are RX then the restore");
        check(!swr_sweep_active(), "idle again, so another sweep can start");
        check(swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now) == SWR_SWEEP_START_OK,
              "a new sweep starts after one finished");
        g.log.clear();
        run();
    }

    // The median of three reads, so a transient spike does not become the point.
    {
        Seen seen;
        Fake f = qmx_with({{7000, 7300}});
        f.swr_script = {140, 187, 141};
        reset(f);
        const swr_sweep_range_t r[] = {{7100, 7100, 5}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        run();
        check(count_kind(seen, SWR_SWEEP_EVENT_POINT) == 1 && seen.events[0].swr_hundredths == 141,
              "a 1.87 spike between 1.40 and 1.41 reads 1.41");
    }

    // A band the radio does not have: both edges refused, nothing keyed, the next range still runs.
    {
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{3500, 4000, 25}, {7000, 7040, 20}};
        swr_sweep_start(r, 2, 14074000, &k_io, on_event, &seen, g_now);
        run();
        check(seen.events.size() == 5, "skipped, three points, done");
        check(seen.events[0].kind == SWR_SWEEP_EVENT_BAND_SKIPPED && seen.events[0].range_index == 0,
              "the radio's missing band is reported as skipped");
        check(count_kind(seen, SWR_SWEEP_EVENT_POINT) == 3 && seen.events[1].range_index == 1, "the next range is measured");
        check(count_cmd("FA3500000") == 1 && count_cmd("FA4000000") == 1, "only the two edges of the missing band were tried");

        Seen only;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t missing[] = {{50000, 54000, 250}};
        swr_sweep_start(missing, 1, 14074000, &k_io, on_event, &only, g_now);
        run();
        check(!g.ever_keyed, "a band the radio lacks never keys the transmitter");
        check(only.events.size() == 2 && only.events[1].kind == SWR_SWEEP_EVENT_DONE, "and the sweep still finishes");
    }

    // The radio has the band but refuses its first frequency (the bench's 80 m): that point only is lost.
    {
        Seen seen;
        reset(qmx_with({{7010, 7100}}));
        const swr_sweep_range_t r[] = {{7000, 7100, 50}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        run();
        check(count_kind(seen, SWR_SWEEP_EVENT_POINT) == 2, "7000 is refused, 7050 and 7100 are measured");
        check(seen.events[0].freq_hz == 7050000 && seen.events[1].freq_hz == 7100000,
              "no point is reported for the frequency the radio refused");
        check(count_kind(seen, SWR_SWEEP_EVENT_BAND_SKIPPED) == 0, "and the band is not reported skipped");
    }

    // The bench bug: MD8 snapped FA back to the old frequency. A reading taken there is not the point.
    {
        Seen seen;
        Fake f = qmx_with({{7000, 7300}});
        f.fa_drifts_when_keyed = true;
        reset(f);
        const swr_sweep_range_t r[] = {{7000, 7040, 20}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        run();
        check(count_kind(seen, SWR_SWEEP_EVENT_POINT) == 0, "a point whose frequency moved while keyed is not reported");
        check(seen.events.back().kind == SWR_SWEEP_EVENT_DONE && !g.keyed, "the sweep still ends, unkeyed");
    }

    // Cancel: the carrier drops at once, then the radio is restored and ABORTED is reported.
    {
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{7000, 7300, 20}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        for (int i = 0; i < 100000 && !g.keyed; ++i) {
            g_now += 10;
            swr_sweep_tick(g_now);
        }
        check(g.keyed, "the test reached a keyed moment");
        swr_sweep_cancel();
        check(!g.keyed, "cancel de-keys before it returns");
        check(swr_sweep_active(), "and the sweep is still finishing");
        run();
        check(seen.events.back().kind == SWR_SWEEP_EVENT_ABORTED &&
                  seen.events.back().abort_reason == SWR_SWEEP_ABORT_CANCELLED, "ABORTED, cancelled");
        check(g.restores == 1 && g.restore_hz == 14074000 && !g.keyed, "the radio is restored once");
        const size_t events = seen.events.size();
        swr_sweep_tick(g_now + 1000);
        check(seen.events.size() == events, "nothing more is reported");
        swr_sweep_cancel();
        check(!swr_sweep_active(), "cancel while idle does nothing");
    }

    // A radio that stops answering: three silent queries in a row end the sweep, unkeyed and restored.
    {
        Seen seen;
        Fake f = qmx_with({{7000, 7300}});
        f.silent = true;
        reset(f);
        const swr_sweep_range_t r[] = {{7000, 7300, 20}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        check(run(), "a silent radio does not hang the sweep");
        check(seen.events.size() == 1 && seen.events[0].kind == SWR_SWEEP_EVENT_ABORTED &&
                  seen.events[0].abort_reason == SWR_SWEEP_ABORT_CAT_LOST, "ABORTED, CAT lost");
        check(g.restores == 1 && !g.keyed, "and the radio is restored");
    }

    // A CAT call that fails while keying: abort, and make sure it is unkeyed.
    {
        Seen seen;
        Fake f = qmx_with({{7000, 7300}});
        f.fail_key = true;
        reset(f);
        const swr_sweep_range_t r[] = {{7000, 7040, 20}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        run();
        check(seen.events.size() == 1 && seen.events[0].kind == SWR_SWEEP_EVENT_ABORTED &&
                  seen.events[0].abort_reason == SWR_SWEEP_ABORT_CAT_LOST, "a failed CAT call aborts the sweep");
        check(g.log[g.log.size() - 2] == "RX" && !g.keyed, "after a last RX;");
    }

    // Another task's requests: handed to the ticking task, which starts, cancels and reports.
    {
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{7000, 7040, 20}};
        check(swr_sweep_request_start(r, 1, 14074000, &k_io, on_event, &seen) == SWR_SWEEP_START_OK, "a request is accepted");
        check(swr_sweep_busy() && !swr_sweep_active() && g.log.empty(), "busy at once, but nothing is sent until a tick");
        check(swr_sweep_request_start(r, 1, 14074000, &k_io, on_event, &seen) == SWR_SWEEP_START_BUSY,
              "a second request while one waits is refused");
        check(run(), "the requested sweep runs to its end");
        check(count_kind(seen, SWR_SWEEP_EVENT_POINT) == 3 && seen.events.back().kind == SWR_SWEEP_EVENT_DONE,
              "with its points and a DONE");
        check(!swr_sweep_busy() && g.restores == 1, "and it is not busy afterwards");

        const swr_sweep_range_t bad[] = {{7000, 7040, 0}};
        check(swr_sweep_request_start(bad, 1, 14074000, &k_io, on_event, &seen) == SWR_SWEEP_START_BAD_RANGE,
              "a bad range is refused up front");
        check(swr_sweep_request_start(r, 1, 14074000, nullptr, on_event, &seen) == SWR_SWEEP_START_UNSUPPORTED,
              "a radio that cannot sweep is refused up front");
        check(!swr_sweep_busy(), "and a refused request does not leave it busy");
    }
    {
        // Cancelled before the first tick: nothing is ever sent.
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{7000, 7040, 20}};
        swr_sweep_request_start(r, 1, 14074000, &k_io, on_event, &seen);
        swr_sweep_request_cancel();
        run();
        check(g.log.empty() && !g.ever_keyed, "a request cancelled before it starts sends nothing");
        check(seen.events.size() == 1 && seen.events[0].kind == SWR_SWEEP_EVENT_ABORTED &&
                  seen.events[0].abort_reason == SWR_SWEEP_ABORT_CANCELLED, "and is reported cancelled");
        check(!swr_sweep_busy(), "and is no longer busy");
    }
    {
        // Cancelled mid-sweep: de-keyed on the next tick.
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{7000, 7300, 20}};
        swr_sweep_request_start(r, 1, 14074000, &k_io, on_event, &seen);
        for (int i = 0; i < 100000 && !g.keyed; ++i) {
            g_now += 10;
            swr_sweep_tick(g_now);
        }
        check(g.keyed, "keyed mid-sweep");
        swr_sweep_request_cancel();
        g_now += 10;
        swr_sweep_tick(g_now);
        check(!g.keyed, "the tick after a cancel request has de-keyed");
        run();
        check(seen.events.back().kind == SWR_SWEEP_EVENT_ABORTED &&
                  seen.events.back().abort_reason == SWR_SWEEP_ABORT_CANCELLED && g.restores == 1,
              "reported cancelled, radio restored");
    }
    {
        // The tick stops for two seconds while keyed: the next tick de-keys and aborts.
        Seen seen;
        reset(qmx_with({{7000, 7300}}));
        const swr_sweep_range_t r[] = {{7000, 7300, 20}};
        swr_sweep_start(r, 1, 14074000, &k_io, on_event, &seen, g_now);
        for (int i = 0; i < 100000 && !(g.keyed && g.log.back() == "SW?"); ++i) {
            g_now += 10;
            swr_sweep_tick(g_now);
        }
        check(g.keyed, "keyed and reading");
        g_now += 2000;
        swr_sweep_tick(g_now);
        check(!g.keyed, "a tick two seconds late de-keys at once");
        run();
        check(seen.events.back().kind == SWR_SWEEP_EVENT_ABORTED &&
                  seen.events.back().abort_reason == SWR_SWEEP_ABORT_STALLED && g.restores == 1,
              "and the sweep is reported stalled, the radio restored");
    }

    if (g_fails == 0) {
        printf("PASS: swr_sweep\n");
    }
    return g_fails == 0 ? 0 : 1;
}
