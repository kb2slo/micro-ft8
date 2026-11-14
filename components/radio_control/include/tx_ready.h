#pragma once

// Whether the station may queue something that will transmit: a CQ or a reply.
// Refused unless the radio is under CAT, its audio is arriving and the decoder
// runs, so nothing waits in the queue to key by itself when a radio turns up
// (docs/architecture/autoseq.md, Where entries come from). Returns the reason,
// in the radio bar's words where it has them (docs/architecture/webapp.md, The
// radio bar), or nullptr when it may. Header only, so host_mock tests it.
inline const char* tx_ready_refusal(bool cat_ready, bool audio_arriving, bool decode_failed) {
    if (!cat_ready && !audio_arriving) return "No radio";
    if (!cat_ready) return "CAT not ready";  // Tune's word for it (porta_host_tune())
    if (!audio_arriving) return "no audio from the radio";
    if (decode_failed) return "Not decoding";
    return nullptr;
}
