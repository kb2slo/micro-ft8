// The heard ring (heard_ring.h).

#include "heard_ring.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

bool power_of_two(size_t n) { return n != 0 && (n & (n - 1)) == 0; }

// Upper case, <> stripped. False for no call: empty, too long, or a hashed call not resolved ("...").
bool normalize(const char* src, char out[kHeardCallChars + 1])
{
    if (!src) return false;
    if (*src == '<') ++src;
    size_t len = strlen(src);
    if (len > 0 && src[len - 1] == '>') --len;
    if (len == 0 || len > kHeardCallChars) return false;
    if (len == 3 && memcmp(src, "...", 3) == 0) return false;
    for (size_t i = 0; i < len; ++i) out[i] = (char)toupper((unsigned char)src[i]);
    out[len] = '\0';
    return true;
}

// FNV-1a.
uint32_t hash(const char* s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

size_t probe_len(const HeardRing* ring)
{
    return ring->buf.call_count < kHeardProbe ? ring->buf.call_count : kHeardProbe;
}

HeardCallSlot* find(const HeardRing* ring, const char* call)
{
    const size_t mask = ring->buf.call_count - 1;
    const size_t start = hash(call) & mask;
    for (size_t i = 0; i < probe_len(ring); ++i) {
        HeardCallSlot* s = &ring->buf.calls[(start + i) & mask];
        if (s->call[0] == '\0') return nullptr;  // no slot is freed, so a call is never past a free one
        if (strcmp(s->call, call) == 0) return s;
    }
    return nullptr;
}

// A slot for a call not in the table: a free one in its window, else the one whose newest record is
// oldest (written over already, or soonest to be).
HeardCallSlot* claim(HeardRing* ring, const char* call)
{
    const size_t mask = ring->buf.call_count - 1;
    const size_t start = hash(call) & mask;
    HeardCallSlot* stalest = nullptr;
    for (size_t i = 0; i < probe_len(ring); ++i) {
        HeardCallSlot* s = &ring->buf.calls[(start + i) & mask];
        if (s->call[0] == '\0') return s;
        if (!stalest || s->newest < stalest->newest) stalest = s;
    }
    if (heard_ring_at(ring, stalest->newest)) ++ring->evicted;
    return stalest;
}

}  // namespace

bool heard_ring_init(HeardRing* ring, const HeardRingBuffers& buffers)
{
    if (!ring) return false;
    *ring = HeardRing{};
    if (!buffers.records || !buffers.calls || !buffers.scratch || !power_of_two(buffers.record_count) ||
        !power_of_two(buffers.call_count))
        return false;
    ring->buf = buffers;
    memset(buffers.records, 0, buffers.record_count * sizeof(HeardRecord));
    memset(buffers.calls, 0, buffers.call_count * sizeof(HeardCallSlot));
    return true;
}

namespace {

uint32_t secs_of(const HeardRecord* r) { return (uint32_t)((uint64_t)r->slot_idx * r->slot_len_ds / 10); }

// The records of a summary, newest first: a call's chain, or every record.
struct Walk {
    const HeardRing* ring;
    bool by_call;
    const HeardRecord* r;
    const HeardRecord* next()
    {
        const HeardRecord* cur = r;
        if (cur) r = by_call ? heard_ring_at(ring, cur->prev) : heard_ring_at(ring, cur->seq - 1);
        return cur;
    }
};

// The lower median of a sorted run of scratch entries, by their low byte (SNR + 128).
int8_t median_of(const uint16_t* run, size_t n) { return (int8_t)((int)(run[(n - 1) / 2] & 0xFF) - 128); }

}  // namespace

uint32_t heard_ring_oldest(const HeardRing* ring)
{
    if (!ring->buf.records || ring->newest == 0) return 0;
    const uint32_t cap = (uint32_t)ring->buf.record_count;
    return ring->newest > cap ? ring->newest - cap + 1 : 1;
}

const HeardRecord* heard_ring_at(const HeardRing* ring, uint32_t seq)
{
    if (!ring->buf.records || seq == 0 || seq > ring->newest || seq < heard_ring_oldest(ring)) return nullptr;
    const HeardRecord* r = &ring->buf.records[seq & (ring->buf.record_count - 1)];
    return r->seq == seq ? r : nullptr;
}

uint32_t heard_ring_add(HeardRing* ring, const HeardDecode& d)
{
    if (!ring->buf.records) return 0;
    const uint32_t seq = ++ring->newest;
    HeardRecord& r = ring->buf.records[seq & (ring->buf.record_count - 1)];
    r = HeardRecord{};
    const char* text = d.text ? d.text : "";
    size_t len = strlen(text);
    if (len > kHeardTextChars) {
        len = kHeardTextChars;
        ++ring->too_long;
    }
    memcpy(r.text, text, len);
    r.text[len] = '\0';
    r.seq = seq;
    r.slot_idx = d.slot_idx;
    r.dial_hz = d.dial_hz;
    r.offset_hz = d.offset_hz;
    r.dt_centis = d.dt_centis;
    r.snr = d.snr;
    r.slot_len_ds = d.slot_len_ds;
    r.flags = d.is_cq ? kHeardCq : 0;

    char call[kHeardCallChars + 1];
    if (normalize(d.sender, call)) {
        HeardCallSlot* s = find(ring, call);
        if (s) {
            r.prev = heard_ring_at(ring, s->newest) ? s->newest : 0;
        } else {
            s = claim(ring, call);
            memcpy(s->call, call, sizeof(s->call));
        }
        s->newest = seq;
    }
    return seq;
}

HeardPage heard_ring_page(const HeardRing* ring, const char* call, uint32_t before, const HeardRecord** out,
                          size_t max)
{
    HeardPage page{};
    if (!ring->buf.records || ring->newest == 0) return page;
    const uint32_t oldest = heard_ring_oldest(ring);
    if (before != 0 && before <= oldest) {
        page.lost = oldest > 1;  // older records were written over
        return page;
    }
    const uint32_t first = before == 0 || before > ring->newest ? ring->newest : before - 1;

    char want[kHeardCallChars + 1];
    if (call && call[0]) {
        if (!normalize(call, want)) return page;
        const HeardCallSlot* s = find(ring, want);
        if (!s) return page;
        const HeardRecord* r = heard_ring_at(ring, s->newest);
        while (r && r->seq > first) r = heard_ring_at(ring, r->prev);
        for (; r; r = heard_ring_at(ring, r->prev)) {
            if (page.count == max) {
                page.more = true;
                break;
            }
            out[page.count++] = r;
        }
        return page;
    }

    for (uint32_t seq = first; seq >= oldest && seq != 0; --seq) {
        if (page.count == max) {
            page.more = true;
            break;
        }
        out[page.count++] = heard_ring_at(ring, seq);
    }
    return page;
}

HeardSummary heard_ring_summary(const HeardRing* ring, const char* call, uint8_t (*band_of)(uint32_t dial_hz))
{
    HeardSummary sum{};
    if (!ring->buf.records || ring->newest == 0) return sum;
    const uint32_t oldest = heard_ring_oldest(ring);
    sum.wrapped = oldest > 1;
    for (uint32_t seq = oldest; seq != 0 && seq <= ring->newest; ++seq) {
        const HeardRecord* r = heard_ring_at(ring, seq);
        if (r && r->slot_idx) {
            sum.since_secs = secs_of(r);
            break;
        }
    }
    for (uint32_t seq = ring->newest; seq >= oldest && seq != 0; --seq) {
        const HeardRecord* r = heard_ring_at(ring, seq);
        if (r && r->slot_idx) {
            sum.hour_end = secs_of(r) / 3600;
            break;
        }
    }

    Walk walk{ring, false, nullptr};
    if (call && call[0]) {
        char want[kHeardCallChars + 1];
        if (!normalize(call, want)) return sum;
        const HeardCallSlot* s = find(ring, want);
        if (!s) return sum;
        walk = {ring, true, heard_ring_at(ring, s->newest)};
    } else {
        walk = {ring, false, heard_ring_at(ring, ring->newest)};
    }

    // One walk: the counts and times, and into scratch each decode's group (high 4 bits), hour (next 4,
    // kHeardHours for none) and SNR + 128 (low 8).
    uint16_t* scratch = ring->buf.scratch;
    size_t n = 0;
    for (const HeardRecord* r = walk.next(); r; r = walk.next()) {
        ++sum.count;
        const uint8_t band = band_of ? band_of(r->dial_hz) : 0;
        size_t g = 0;
        while (g < sum.groups && !(sum.group[g].band == band && sum.group[g].slot_len_ds == r->slot_len_ds)) ++g;
        if (g == sum.groups) {
            if (sum.groups == kHeardSummaryGroups) {
                sum.more_groups = true;
                continue;
            }
            HeardGroupSummary& ng = sum.group[sum.groups++];
            ng.band = band;
            ng.slot_len_ds = r->slot_len_ds;
            ng.snr_now = r->snr;
            ng.snr_best = r->snr;
            for (int8_t& h : ng.hourly) h = kHeardNoSnr;
        }
        HeardGroupSummary& gs = sum.group[g];
        ++gs.count;
        if (r->snr > gs.snr_best) gs.snr_best = r->snr;
        size_t hour = kHeardHours;
        if (r->slot_idx) {
            const uint32_t secs = secs_of(r);
            if (!gs.last_secs) gs.last_secs = secs;
            gs.first_secs = secs;
            const uint32_t back = sum.hour_end - std::min(sum.hour_end, secs / 3600);
            if (secs / 3600 <= sum.hour_end && back < kHeardHours) hour = kHeardHours - 1 - back;
        }
        if (n < ring->buf.record_count)
            scratch[n++] = (uint16_t)((g << 12) | (hour << 8) | (uint8_t)((int)r->snr + 128));
    }

    // By group and SNR: each group's median.
    std::sort(scratch, scratch + n, [](uint16_t a, uint16_t b) {
        return ((a >> 12) << 8 | (a & 0xFF)) < ((b >> 12) << 8 | (b & 0xFF));
    });
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && (scratch[j] >> 12) == (scratch[i] >> 12)) ++j;
        sum.group[scratch[i] >> 12].snr_median = median_of(scratch + i, j - i);
        i = j;
    }
    // By group, hour and SNR: each hour's.
    std::sort(scratch, scratch + n);
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && (scratch[j] >> 8) == (scratch[i] >> 8)) ++j;
        const size_t hour = (scratch[i] >> 8) & 0x0F;
        if (hour < kHeardHours) sum.group[scratch[i] >> 12].hourly[hour] = median_of(scratch + i, j - i);
        i = j;
    }
    return sum;
}
