// heard_ring: the wrap, the lookup by sender call, and paging by cursor (components/heard_ring/include/heard_ring.h).
//
// Checked against a second model written here from the header: every decode added, kept in a plain list. A
// page of the ring must equal the newest matching decodes of that list still inside the ring's window.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "heard_ring.h"

static int g_fails = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            ++g_fails;                                           \
        }                                                        \
    } while (0)

struct Rig {
    std::vector<HeardRecord> records;
    std::vector<HeardCallSlot> calls;
    std::vector<uint16_t> scratch;
    HeardRing ring{};
    Rig(size_t record_count, size_t call_count)
        : records(record_count), calls(call_count), scratch(record_count)
    {
        CHECK(heard_ring_init(&ring, {records.data(), records.size(), calls.data(), calls.size(), scratch.data()}),
              "init");
    }
};

struct Added {
    uint32_t seq;
    std::string text;
    std::string sender;  // normalized; empty when unindexed
    uint32_t slot;
    uint32_t dial_hz;
    uint8_t slot_len_ds;
    int8_t snr;
};

static std::string upper(std::string s)
{
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static uint32_t add(Rig& rig, std::vector<Added>& model, const std::string& text, const std::string& sender,
                    uint32_t slot = 100, int8_t snr = -7, uint32_t dial_hz = 14074000, uint8_t slot_len_ds = 150)
{
    HeardDecode d{};
    d.text = text.c_str();
    d.sender = sender.c_str();
    d.slot_idx = slot;
    d.dial_hz = dial_hz;
    d.offset_hz = 1500;
    d.dt_centis = -12;
    d.snr = snr;
    d.slot_len_ds = slot_len_ds;
    d.is_cq = text.rfind("CQ ", 0) == 0;
    const uint32_t seq = heard_ring_add(&rig.ring, d);
    std::string norm = sender;
    if (!norm.empty() && norm.front() == '<') norm.erase(0, 1);
    if (!norm.empty() && norm.back() == '>') norm.pop_back();
    if (norm == "...") norm.clear();
    model.push_back({seq, text, upper(norm), slot, dial_hz, slot_len_ds, snr});
    return seq;
}

// Every page of `call` (empty: all), `take` at a time, against the model.
static void check_pages(const Rig& rig, const std::vector<Added>& model, const std::string& call, size_t take)
{
    const uint32_t oldest = heard_ring_oldest(&rig.ring);
    std::vector<uint32_t> want;
    for (auto it = model.rbegin(); it != model.rend(); ++it)
        if (it->seq >= oldest && (call.empty() || it->sender == upper(call))) want.push_back(it->seq);

    std::vector<uint32_t> got;
    std::vector<const HeardRecord*> out(take);
    uint32_t before = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        HeardPage p = heard_ring_page(&rig.ring, call.c_str(), before, out.data(), take);
        CHECK(!p.lost, "no page of a walk from the newest is lost (call %s)", call.c_str());
        for (size_t i = 0; i < p.count; ++i) got.push_back(out[i]->seq);
        if (!p.more) break;
        CHECK(p.count == take, "a page with more is full");
        before = out[p.count - 1]->seq;
    }
    CHECK(got == want, "pages of '%s' by %zu: got %zu records, want %zu", call.c_str(), take, got.size(),
          want.size());
}

static void test_init()
{
    HeardRecord rec[4];
    HeardCallSlot slots[4];
    uint16_t scratch[4];
    HeardRing ring{};
    CHECK(!heard_ring_init(&ring, {rec, 3, slots, 4, scratch}), "a record count not a power of two is refused");
    CHECK(!heard_ring_init(&ring, {rec, 4, slots, 0, scratch}), "no call slots is refused");
    CHECK(!heard_ring_init(&ring, {nullptr, 4, slots, 4, scratch}), "no records is refused");
    CHECK(!heard_ring_init(&ring, {rec, 4, slots, 4, nullptr}), "no scratch is refused");
    const HeardRecord* out[1];
    CHECK(heard_ring_page(&ring, "", 0, out, 1).count == 0, "a refused ring pages nothing");
    CHECK(heard_ring_add(&ring, HeardDecode{}) == 0, "a refused ring adds nothing");
    CHECK(heard_ring_init(&ring, {rec, 4, slots, 4, scratch}), "powers of two are taken");
    CHECK(heard_ring_summary(&ring, "K1ABC", nullptr).count == 0, "an empty ring sums to nothing");
    CHECK(heard_ring_oldest(&ring) == 0 && heard_ring_page(&ring, "", 0, out, 1).count == 0, "empty");
}

static void test_fields()
{
    Rig rig(8, 16);
    std::vector<Added> model;
    add(rig, model, "CQ POTA K1ABC FN42", "K1ABC", 119721033);
    const HeardRecord* r = heard_ring_at(&rig.ring, 1);
    CHECK(r && strcmp(r->text, "CQ POTA K1ABC FN42") == 0, "text kept");
    CHECK(r && r->slot_idx == 119721033 && r->dial_hz == 14074000 && r->offset_hz == 1500 &&
              r->dt_centis == -12 && r->snr == -7 && r->slot_len_ds == 150 && (r->flags & kHeardCq) &&
              r->prev == 0,
          "fields kept");

    const std::string longest(kHeardTextChars, 'A');
    add(rig, model, longest, "");
    CHECK(strcmp(heard_ring_at(&rig.ring, 2)->text, longest.c_str()) == 0 && rig.ring.too_long == 0,
          "a message of WSJT-X's longest is kept whole");
    add(rig, model, longest + "BC", "");
    CHECK(strcmp(heard_ring_at(&rig.ring, 3)->text, longest.c_str()) == 0 && rig.ring.too_long == 1,
          "a longer one is cut to it and counted");
}

static void test_wrap()
{
    Rig rig(16, 64);
    std::vector<Added> model;
    for (int i = 0; i < 40; ++i) add(rig, model, "CQ W" + std::to_string(i) + "X EM10", "W" + std::to_string(i % 5) + "X");
    CHECK(rig.ring.newest == 40 && heard_ring_oldest(&rig.ring) == 25, "the ring holds the newest 16");
    CHECK(heard_ring_at(&rig.ring, 24) == nullptr && heard_ring_at(&rig.ring, 25) != nullptr &&
              heard_ring_at(&rig.ring, 41) == nullptr && heard_ring_at(&rig.ring, 0) == nullptr,
          "a record written over is gone");
    for (size_t take : {1, 3, 16, 100}) {
        check_pages(rig, model, "", take);
        for (int c = 0; c < 5; ++c) check_pages(rig, model, "W" + std::to_string(c) + "X", take);
    }
    const HeardRecord* out[4];
    HeardPage p = heard_ring_page(&rig.ring, "", 25, out, 4);
    CHECK(p.count == 0 && p.lost, "a cursor at the oldest, with older ones written over, says lost");
    p = heard_ring_page(&rig.ring, "W1X", 10, out, 4);
    CHECK(p.count == 0 && p.lost, "a cursor before the ring says lost, by call too");

    Rig fresh(16, 64);
    std::vector<Added> m2;
    for (int i = 0; i < 5; ++i) add(fresh, m2, "x", "K1ABC");
    p = heard_ring_page(&fresh.ring, "", 1, out, 4);
    CHECK(p.count == 0 && !p.lost && !p.more, "the end of a ring that never wrapped is not lost");
}

static void test_call_names()
{
    Rig rig(32, 64);
    std::vector<Added> model;
    add(rig, model, "W9XYZ <K1ABC> -05", "<K1ABC>");
    add(rig, model, "CQ k1abc FN42", "k1abc");
    add(rig, model, "W9XYZ <...> RR73", "<...>");
    add(rig, model, "W9XYZ <...> RR73", "...");
    add(rig, model, "CQ", "");
    const HeardRecord* out[8];
    HeardPage p = heard_ring_page(&rig.ring, "<k1ABC>", 0, out, 8);
    CHECK(p.count == 2 && out[0]->seq == 2 && out[1]->seq == 1, "<> and case are one call");
    CHECK(heard_ring_page(&rig.ring, "...", 0, out, 8).count == 0, "an unresolved hashed call names no one");
    CHECK(heard_ring_page(&rig.ring, "", 0, out, 8).count == 5, "every decode is in the ring, named or not");
    CHECK(heard_ring_page(&rig.ring, "N0BODY", 0, out, 8).count == 0, "a call never heard has none");
    CHECK(heard_ring_page(&rig.ring, "ABCDEFGHIJKLMNOP", 0, out, 8).count == 0, "a call too long has none");
}

// A call whose records were all written over, heard again, starts a new chain.
static void test_stale_call()
{
    Rig rig(8, 64);
    std::vector<Added> model;
    add(rig, model, "CQ K1ABC FN42", "K1ABC");
    for (int i = 0; i < 10; ++i) add(rig, model, "CQ N" + std::to_string(i) + "X EM10", "N" + std::to_string(i) + "X");
    const uint32_t seq = add(rig, model, "CQ K1ABC FN42", "K1ABC");
    CHECK(heard_ring_at(&rig.ring, seq)->prev == 0, "no link to a record written over");
    check_pages(rig, model, "K1ABC", 4);
    CHECK(rig.ring.evicted == 0, "no live call was evicted");
}

// With more calls than a window of slots, the stalest call loses its slot: its records stay in the ring
// but are no longer found by call, and the count says so.
static void test_eviction()
{
    Rig rig(64, 4);  // a window of 4 slots
    std::vector<Added> model;
    for (int i = 0; i < 5; ++i) add(rig, model, "CQ", "C" + std::to_string(i) + "A");
    CHECK(rig.ring.evicted == 1, "the fifth call took a live call's slot");
    const HeardRecord* out[8];
    CHECK(heard_ring_page(&rig.ring, "C0A", 0, out, 8).count == 0, "the stalest call is no longer found");
    for (int i = 1; i < 5; ++i)
        CHECK(heard_ring_page(&rig.ring, ("C" + std::to_string(i) + "A").c_str(), 0, out, 8).count == 1,
              "the others are");
    CHECK(heard_ring_page(&rig.ring, "", 0, out, 8).count == 5, "every record is still in the ring");
}

// Random traffic over a small ring and a roomy table, every call's pages against the model.
static void test_random()
{
    srand(12345);
    Rig rig(256, 1024);
    std::vector<Added> model;
    std::vector<std::string> calls;
    for (int i = 0; i < 60; ++i) calls.push_back("K" + std::to_string(i) + "QQ");
    for (int i = 0; i < 2000; ++i) {
        const std::string& c = calls[rand() % calls.size()];
        add(rig, model, "CQ " + c + " EM10", rand() % 20 == 0 ? std::string("<...>") : c, 100 + i / 20);
    }
    CHECK(rig.ring.evicted == 0, "a roomy table evicts no one");
    check_pages(rig, model, "", 7);
    for (const std::string& c : calls) check_pages(rig, model, c, 3);
}

static uint8_t band_of(uint32_t hz) { return (uint8_t)(hz / 1000000); }  // MHz stands in for the band

// FT8 slots of an hour: hour h's slot k (240 FT8 slots an hour).
static uint32_t ft8_slot(uint32_t hour, uint32_t k) { return hour * 240 + k; }

static void test_summary_example()
{
    Rig rig(64, 64);
    std::vector<Added> model;
    const uint32_t h = 498837;  // an hour in 2026
    // 20 m FT8: -20, -18 in hour h-2; nothing in h-1; -9, -7, -3 in h. 40 m FT4: one at -15 in h-1.
    add(rig, model, "CQ K1ABC FN42", "K1ABC", ft8_slot(h - 2, 3), -20);
    add(rig, model, "CQ K1ABC FN42", "K1ABC", ft8_slot(h - 2, 9), -18);
    add(rig, model, "CQ K1ABC FN42", "K1ABC", 2 * ft8_slot(h - 1, 5), -15, 7047000, 75);
    add(rig, model, "W9XYZ K1ABC -05", "K1ABC", ft8_slot(h, 1), -9);
    add(rig, model, "W9XYZ K1ABC RR73", "K1ABC", ft8_slot(h, 2), -3);
    add(rig, model, "CQ N0CALL EM10", "N0CALL", ft8_slot(h, 3), 4);
    add(rig, model, "CQ K1ABC FN42", "K1ABC", ft8_slot(h, 4), -7);
    add(rig, model, "CQ K1ABC FN42", "K1ABC", 0, -1);  // no clock: counts, no time, no hour

    HeardSummary s = heard_ring_summary(&rig.ring, "k1abc", band_of);
    CHECK(s.count == 7 && s.groups == 2 && !s.more_groups && !s.wrapped, "7 records in two groups");
    CHECK(s.hour_end == h, "the series ends on the ring's newest timed hour");
    CHECK(s.since_secs == ft8_slot(h - 2, 3) * 15, "since: the ring's oldest timed record");
    const HeardGroupSummary& a = s.group[0];
    CHECK(a.band == 14 && a.slot_len_ds == 150 && a.count == 6, "20 m FT8 first, its newest decode newest");
    CHECK(a.snr_now == -1 && a.snr_best == -1, "now and best");
    // SNRs -20 -18 -9 -7 -3 -1: lower median -9.
    CHECK(a.snr_median == -9, "median %d", a.snr_median);
    CHECK(a.first_secs == ft8_slot(h - 2, 3) * 15 && a.last_secs == ft8_slot(h, 4) * 15, "first and last");
    CHECK(a.hourly[kHeardHours - 1] == -7 && a.hourly[kHeardHours - 2] == kHeardNoSnr &&
              a.hourly[kHeardHours - 3] == -20 && a.hourly[0] == kHeardNoSnr,
          "hourly medians, a gap where it was not heard");
    const HeardGroupSummary& b = s.group[1];
    CHECK(b.band == 7 && b.slot_len_ds == 75 && b.count == 1 && b.snr_median == -15 &&
              b.hourly[kHeardHours - 2] == -15 && b.first_secs == ft8_slot(h - 1, 5) * 15,
          "40 m FT4, timed by its own 7.5 s slots");

    CHECK(heard_ring_summary(&rig.ring, "", band_of).count == 8, "with no call, every record");
    CHECK(heard_ring_summary(&rig.ring, "W9XYZ", band_of).count == 0, "a call never the sender has none");
}

// The summary against the model, over random traffic that wraps.
static void test_summary_random()
{
    srand(777);
    Rig rig(512, 1024);
    std::vector<Added> model;
    const uint32_t h0 = 498800;
    const uint32_t dials[] = {14074000, 7074000, 21074000};
    for (int i = 0; i < 3000; ++i) {
        const std::string c = "K" + std::to_string(rand() % 8) + "ZZ";
        const uint32_t slot = ft8_slot(h0, 0) + (uint32_t)i * 3;  // about 13 hours over the run
        add(rig, model, "CQ " + c, c, slot, (int8_t)(rand() % 50 - 30), dials[rand() % 3]);
    }
    const uint32_t oldest = heard_ring_oldest(&rig.ring);
    for (int k = 0; k < 8; ++k) {
        const std::string c = "K" + std::to_string(k) + "ZZ";
        HeardSummary s = heard_ring_summary(&rig.ring, c.c_str(), band_of);
        CHECK(s.wrapped, "wrapped");
        uint32_t count = 0;
        for (const Added& a : model) count += a.seq >= oldest && a.sender == c;
        CHECK(s.count == count, "%s count %u, want %u", c.c_str(), s.count, count);
        for (size_t g = 0; g < s.groups; ++g) {
            const HeardGroupSummary& gs = s.group[g];
            std::vector<int> all;
            std::vector<int> hours[kHeardHours];
            for (const Added& a : model) {
                if (a.seq < oldest || a.sender != c || band_of(a.dial_hz) != gs.band) continue;
                all.push_back(a.snr);
                const uint32_t hr = a.slot * 15 / 3600;
                if (hr <= s.hour_end && s.hour_end - hr < kHeardHours) hours[kHeardHours - 1 - (s.hour_end - hr)].push_back(a.snr);
            }
            std::sort(all.begin(), all.end());
            CHECK(gs.count == all.size() && gs.snr_median == all[(all.size() - 1) / 2] && gs.snr_best == all.back(),
                  "%s group %zu count, median and best", c.c_str(), g);
            for (size_t hr = 0; hr < kHeardHours; ++hr) {
                std::sort(hours[hr].begin(), hours[hr].end());
                const int want = hours[hr].empty() ? kHeardNoSnr : hours[hr][(hours[hr].size() - 1) / 2];
                CHECK(gs.hourly[hr] == want, "%s group %zu hour %zu: %d, want %d", c.c_str(), g, hr, gs.hourly[hr], want);
            }
        }
    }
}

// More bands and modes than the summary holds: the rest only count.
static void test_summary_groups()
{
    Rig rig(64, 64);
    std::vector<Added> model;
    for (uint32_t i = 0; i < kHeardSummaryGroups + 2; ++i) add(rig, model, "CQ K1ABC", "K1ABC", 100 + i, -5, (i + 1) * 1000000);
    HeardSummary s = heard_ring_summary(&rig.ring, "K1ABC", band_of);
    CHECK(s.count == kHeardSummaryGroups + 2 && s.groups == kHeardSummaryGroups && s.more_groups, "capped, counted");
    CHECK(s.group[0].band == kHeardSummaryGroups + 2, "the newest band first");
}

int main()
{
    test_init();
    test_fields();
    test_wrap();
    test_call_names();
    test_stale_call();
    test_eviction();
    test_random();
    test_summary_example();
    test_summary_random();
    test_summary_groups();
    if (g_fails) {
        fprintf(stderr, "heard_ring: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("heard_ring: all passed\n");
    return 0;
}
