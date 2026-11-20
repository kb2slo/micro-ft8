#pragma once

#include <cstring>

// Whether `call` can be the station's own, so that we may transmit at all: not a new station's
// placeholder (YOURCALL, getting-started.md) or NOCALL, and 3 to 13 letters, digits and '/' with at
// least one letter and one digit, as every amateur callsign has. A call it refuses is one nobody may
// transmit under: a CQ, a reply, Tune, an SWR sweep and an amplifier prime all wait for a real one.
inline bool tx_call_ok(const char* call) {
    if (!call) return false;
    const size_t n = strlen(call);
    if (n < 3 || n > 13 || strcmp(call, "YOURCALL") == 0 || strcmp(call, "NOCALL") == 0) return false;
    bool letter = false, digit = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = call[i];
        if (c >= 'A' && c <= 'Z') letter = true;
        else if (c >= '0' && c <= '9') digit = true;
        else if (c != '/') return false;
    }
    return letter && digit;
}

// Whether the station may queue something that will transmit: a CQ or a reply.
// Refused unless the radio is under CAT, its audio is arriving and the decoder
// runs, so nothing waits in the queue to key by itself when a radio turns up
// (docs/architecture/autoseq.md, Where entries come from). Returns the reason,
// in the radio bar's words where it has them (docs/architecture/webapp.md, The
// radio bar), or nullptr when it may. Header only, so host_mock tests it.
inline const char* tx_ready_refusal(bool call_ok, bool cat_ready, bool audio_arriving, bool decode_failed) {
    if (!call_ok) return "No callsign set";  // set once, in Settings › Station, before anything else
    if (!cat_ready && !audio_arriving) return "No radio";
    if (!cat_ready) return "CAT not ready";  // Tune's word for it (porta_host_tune())
    if (!audio_arriving) return "no audio from the radio";
    if (decode_failed) return "Not decoding";
    return nullptr;
}
