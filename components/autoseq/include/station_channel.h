#pragma once

#include <cstdint>
#include <cstdio>

#include "decode_types.h"

// The station's channels, and whether the radio is on them (docs/architecture/autoseq.md, The queue). Header
// only, so host_mock tests the rule the main board runs.

// The dial of the station's channel in `mode` on one QMX: FT8's dial on the band, and FT4's beside it is FT8's
// plus the band's FT4 offset, as WSJT-X logs it. An unknown dial (0) stays unknown.
inline uint32_t queue_mode_dial_hz(RxMode mode, uint32_t ft8_dial_hz, int32_t ft4_dial_offset_hz) {
    if (mode != RxMode::FT4 || ft8_dial_hz == 0) {
        return ft8_dial_hz;
    }
    return (uint32_t)((int64_t)ft8_dial_hz + ft4_dial_offset_hz);
}

// What the main board knows when it asks: the FT8 dial it set for the band, the dial the radio last reported
// (dial_history), and whether FT4 is decoded beside FT8 and how far its dial is from FT8's.
struct StationDials {
    uint32_t set_ft8_dial_hz = 0;
    uint32_t reported_dial_hz = 0;
    bool ft4_decoded = false;
    int32_t ft4_dial_offset_hz = 0;
};

// The radio is on the station's dial: it reports the dial the main board set. A dial moved by hand, or one the
// radio refused, belongs to no channel, so nothing is listened to and nothing transmits until it is back.
inline bool station_on_dial(const StationDials& d) {
    return d.set_ft8_dial_hz != 0 && d.reported_dial_hz == d.set_ft8_dial_hz;
}

// The station's channel in `mode`, if it listens to one: FT8's always, FT4's while it is decoded; none while the
// radio is off the station's dial.
inline bool station_channel(const StationDials& d, RxMode mode, RxChannel* out) {
    if (!station_on_dial(d)) return false;
    if (mode == RxMode::FT4 && !d.ft4_decoded) return false;
    if (out) *out = RxChannel{queue_mode_dial_hz(mode, d.set_ft8_dial_hz, d.ft4_dial_offset_hz), mode};
    return true;
}

// Whether the station listens to `channel` now: a queue entry on any other has nothing to transmit on.
inline bool station_listens(const StationDials& d, RxChannel channel) {
    RxChannel mine;
    return station_channel(d, channel.mode, &mine) && mine == channel;
}

// The reason nothing may transmit while the radio is off the station's dial, in the radio bar's words, or
// nullptr while it is on it. "Radio not on 14.074 MHz": the dial in MHz with its trailing zeros dropped.
inline const char* station_off_dial_refusal(const StationDials& d, char* buf, size_t cap) {
    if (d.set_ft8_dial_hz == 0 || station_on_dial(d)) return nullptr;
    char mhz[16];
    snprintf(mhz, sizeof mhz, "%u.%06u", (unsigned)(d.set_ft8_dial_hz / 1000000), (unsigned)(d.set_ft8_dial_hz % 1000000));
    size_t n = 0;
    while (mhz[n]) ++n;
    while (n > 0 && mhz[n - 1] == '0') mhz[--n] = '\0';
    if (n > 0 && mhz[n - 1] == '.') mhz[--n] = '\0';
    snprintf(buf, cap, "Radio not on %s MHz", mhz);
    return buf;
}
