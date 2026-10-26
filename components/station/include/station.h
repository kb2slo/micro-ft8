#pragma once

#include <string>

// Station.txt parse/serialize. No ESP-IDF. Host tests and firmware both link this.
// Radio integers: 0 none, 1 truSDX, 2 QMX, 5 QDX (the values older Station.txt files saved).

static constexpr int kStationBandCount = 12;

// Least time from TX; (PTT) to the first RF of a QMX transmission, for an amplifier
// keyed from the radio's PTT (architecture/radio.md, TX timing on a QMX).
static constexpr int kStationTxMinDelayDefaultMs = 50;
static constexpr int kStationTxMinDelayMaxMs = 500;

struct StationSettings {
    float band_freq[kStationBandCount];
    bool band_freq_set[kStationBandCount];
    float ft4_band_freq[kStationBandCount];
    bool ft4_band_freq_set[kStationBandCount];

    int offset_hz;
    int band_sel;
    int cq_type;
    bool skip_tx1;
    std::string free_text;
    std::string call;
    std::string grid;
    int offset_src;
    int radio;
    int gps_baud;
    std::string comment1;
    std::string ignore_prefixes;
    bool rxtx_log;
    std::string active_bands;
    int tx_min_delay_ms = kStationTxMinDelayDefaultMs;  // 0..kStationTxMinDelayMaxMs
    bool amp_swr_guard = false;  // recover from the QMX's SWR-protection trips (architecture/radio.md)
    bool amp_prime_on_band_change = false;  // brief QMX SWR Tune pulse after an explicit band change
    int tries;  // transmissions per QSO message, and a CQ's to start (autoseq.md, Tries)
    bool protocol_ft4;
    bool serialize_ft4_band_keys;
    // "POTA:US-1234,SOTA:W7O/CC-001" -- the active activation's references,
    // entered in the Station sheet (architecture/webapp.md, "Activation").
    // Empty means no activation. Normalized (program upper-cased, malformed
    // tokens dropped) by station_parse(); see normalize_act_refs() in
    // station.cpp.
    std::string act_refs;
};

void station_settings_init(StationSettings* out);

// Overlay key=value lines onto *io. Unknown keys ignored. date=, time=,
// rtc_comp=, and rtc_sleep_epoch= are ignored (clock is not persisted).
void station_parse(const std::string& text, StationSettings* io);

// Writes the same keys save_station_data() does.
std::string station_serialize(const StationSettings& in);

// True for Station.txt keys CONFIG may set (call, grid, band0…, …). Clock
// lines are not included — the clock is not persisted here.
bool station_key_known(const char* name);

// On activation start (act_refs empty -> non-empty), the CQ type is derived
// from the references themselves -- SOTA if any SOTA reference is present
// (a mixed activation defaults to SOTA), else POTA (architecture/logging.md,
// "CQ type follows the activation"). act_refs is already
// normalize_act_refs()'d ("POTA:"/"SOTA:" upper-case prefixes) by the time
// a caller has it, on either board.
bool act_refs_prefer_sota(const std::string& act_refs);

// True when a CONFIG write of this key needs the VFO already on the
// operating band to take effect right now: band_sel itself (it picks the new
// operating band), or that band's own frequency key under the active
// protocol (band<band_sel> for FT8, ft4_band<band_sel> for FT4). A frequency
// written for a different band, or for the other protocol's table, is a
// config edit that does not need the radio to move.
bool station_key_names_operating_band(const char* key, int band_sel, bool protocol_ft4);

// The CONFIG refusal for such a write when the VFO did not move: "band saved,
// VFO not moved (…)" for band_sel, "frequency saved, VFO not moved (…)" for a
// frequency key, with "TX active" or "CAT not ready" as the reason.
const char* station_vfo_refusal(const char* key, bool tx_active);