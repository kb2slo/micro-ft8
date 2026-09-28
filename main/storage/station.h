#pragma once

#include <string>

// Station.txt parse/serialize. No ESP-IDF. Host tests and firmware both link this.
// Radio integers match main/station_types.h RadioType values.

static constexpr int kStationBandCount = 12;

struct StationSettings {
    float band_freq[kStationBandCount];
    bool band_freq_set[kStationBandCount];
    float ft4_band_freq[kStationBandCount];
    bool ft4_band_freq_set[kStationBandCount];

    int offset_hz;
    int band_sel;
    int cq_type;
    std::string cq_freetext;
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
    int autoseq_max_retry;
    bool protocol_ft4;
    bool serialize_ft4_band_keys;
};

void station_settings_init(StationSettings* out);

// Overlay key=value lines onto *io. Unknown keys ignored. beacon=, date=,
// time=, rtc_comp=, and rtc_sleep_epoch= are ignored (clock is not persisted).
void station_parse(const std::string& text, StationSettings* io);

// Writes the same keys save_station_data() does. No beacon= line.
std::string station_serialize(const StationSettings& in);

// True for Station.txt keys CONFIG may set (call, grid, band0…, …). Beacon and
// clock lines are not included — those are live controls / not persisted here.
bool station_key_known(const char* name);

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