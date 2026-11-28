#pragma once

#include <cstddef>
#include <cstdint>

// The heard ring: every decode the main board makes, newest last, in RAM only, so a page can read what was
// heard while no page listened (docs/architecture/decode.md, The heard ring). No ESP-IDF: the caller supplies
// the memory, so host tests and firmware link the same code. On the P4 the buffers are static, in PSRAM
// (docs/architecture/memory.md). Lost at a restart, by design.
//
// Each record is numbered from 1 (`seq`) in the order it was added; record `seq` sits at `seq & (capacity
// - 1)` until `capacity` newer records have been added, and is then written over. A record names its
// sender's record before it (`prev`), and a table by sender call names each call's newest, so a call's
// decodes are read newest first without a scan. A link to a record written over ends the walk: a record's
// own `seq` says whether it is still the one a link named.

// The longest message WSJT-X writes as text: its packer and unpacker hold a message in 37 characters
// (lib/77bit/packjt77.f90, character*37). The DXpedition rewrite (decode_fields.h) only shortens a message.
inline constexpr size_t kHeardTextChars = 37;
inline constexpr size_t kHeardCallChars = 15;
// How far a call's lookup probes the table from its hash. Bounds a lookup, and an insert of a new call
// takes the stalest slot in this window when none is free (heard_ring_add()).
inline constexpr size_t kHeardProbe = 64;

inline constexpr uint8_t kHeardCq = 0x01;  // the message is a CQ

// One decode, 64 bytes, so 65,536 of them are 4 MB.
struct HeardRecord {
    uint32_t seq;                    // 0: never written
    uint32_t slot_idx;               // the UTC slot its audio was recorded in, as DECODE's; 0 with no clock
    uint32_t dial_hz;                // the dial at the middle of its signal, as DECODE's `fq`
    uint32_t prev;                   // the seq of its sender's record before it; 0 for none
    char text[kHeardTextChars + 1];  // NUL-terminated
    uint16_t offset_hz;
    int16_t dt_centis;
    int8_t snr;
    uint8_t slot_len_ds;  // 150 FT8, 75 FT4: the mode
    uint8_t flags;        // kHeardCq
    uint8_t reserved[3];
};
static_assert(sizeof(HeardRecord) == 64, "a heard record is 64 bytes");

// A sender call's slot in the lookup table.
struct HeardCallSlot {
    char call[kHeardCallChars + 1];  // upper case; empty: a free slot
    uint32_t newest;                 // the seq of its newest record
};

// The caller's memory, for the life of the ring. Both counts must be powers of two. `scratch` holds
// record_count entries: heard_ring_summary() sorts one call's SNRs in it, so its medians are exact however
// many records the call has.
struct HeardRingBuffers {
    HeardRecord* records;
    size_t record_count;
    HeardCallSlot* calls;
    size_t call_count;
    uint16_t* scratch;
};

struct HeardRing {
    HeardRingBuffers buf;
    uint32_t newest;    // the seq of the newest record; 0 while empty
    uint32_t evicted;   // calls whose slot was taken by another while still in the ring: no longer found by call
    uint32_t too_long;  // messages longer than kHeardTextChars, kept cut short (none is expected)
};

// What a caller adds: one decode. `sender` is its sender's call as the decode list has it (field2), with
// or without <> around it; empty, or "..." for a hashed call not resolved, adds the record unindexed.
struct HeardDecode {
    const char* text;
    const char* sender;
    uint32_t slot_idx;
    uint32_t dial_hz;
    uint16_t offset_hz;
    int16_t dt_centis;
    int8_t snr;
    uint8_t slot_len_ds;
    bool is_cq;
};

// False, and the ring unusable, when a count is 0 or not a power of two, or a pointer is null. Clears both
// buffers.
bool heard_ring_init(HeardRing* ring, const HeardRingBuffers& buffers);

// Adds one decode and returns its seq.
uint32_t heard_ring_add(HeardRing* ring, const HeardDecode& decode);

// The oldest seq the ring still holds; 0 while empty.
uint32_t heard_ring_oldest(const HeardRing* ring);

// The record `seq`, or null when the ring no longer holds it (or never did).
const HeardRecord* heard_ring_at(const HeardRing* ring, uint32_t seq);

struct HeardPage {
    size_t count;  // records written to `out`, newest first
    bool more;     // older records match beyond those
    bool lost;     // `before` reached past the oldest record, and older ones were written over
};

// A page of records older than `before` (0: from the newest), newest first, at most `max`. With `call`
// (non-empty, <> ignored, any case) only that sender's records; without, every record.
HeardPage heard_ring_page(const HeardRing* ring, const char* call, uint32_t before, const HeardRecord** out,
                          size_t max);

// What the ring holds of one call, for the card: a line per band and mode, and the median SNR
// of each UTC hour, the newest kHeardHours of them, so the card can draw which way the station is going.
inline constexpr size_t kHeardHours = 10;
inline constexpr size_t kHeardSummaryGroups = 8;
inline constexpr int8_t kHeardNoSnr = -128;  // an hour the call was not heard in

struct HeardGroupSummary {
    uint8_t band;         // band_of()'s index
    uint8_t slot_len_ds;  // the mode
    uint32_t count;
    uint32_t first_secs;  // UTC seconds of the oldest and newest slot heard; 0 with no clock
    uint32_t last_secs;
    int8_t snr_now;       // the newest decode's
    int8_t snr_median;    // the lower median of every decode's
    int8_t snr_best;
    int8_t hourly[kHeardHours];  // oldest first, hourly[kHeardHours - 1] the hour of `hour_end`; kHeardNoSnr
};

struct HeardSummary {
    uint32_t count;       // every record of the call, grouped or not
    size_t groups;
    HeardGroupSummary group[kHeardSummaryGroups];  // by the newest decode in each, newest first
    bool more_groups;     // a band and mode past kHeardSummaryGroups was heard; its records only count
    uint32_t hour_end;    // the UTC hour (seconds / 3600) the series ends on: the ring's newest timed decode
    uint32_t since_secs;  // UTC seconds of the oldest timed record the ring holds, any call; 0 with none
    bool wrapped;         // older records were written over, so "since" is where the ring begins
};

// The summary of `call`'s records (empty: every record). band_of maps a dial to a band index; records
// with no clock (slot_idx 0) count but have no time and no hour.
HeardSummary heard_ring_summary(const HeardRing* ring, const char* call, uint8_t (*band_of)(uint32_t dial_hz));
