#pragma once

#include <string>

// Station.txt parse/serialize. No ESP-IDF. Host tests and firmware both link this.

static constexpr int kStationBandCount = 12;

// Least time from TX; (PTT) to the first RF of a QMX transmission, for an amplifier
// keyed from the radio's PTT (architecture/radio.md, TX timing on a QMX).
static constexpr int kStationTxMinDelayDefaultMs = 50;
static constexpr int kStationTxMinDelayMaxMs = 500;

// Station.txt's radio= values: the QMX keeps Mini-FT8's number; the HL2 is ours (row mg9c, docs/roadmap/hl2-support.md). Anything else
// reads as the QMX.
constexpr int kStationRadioQmx = 2;
constexpr int kStationRadioHl2 = 6;

// Station.txt's amp_ptt= values: how an amplifier is keyed from the QMX's PTT jack, which Settings
// checks against the radio's own per-band PTT outputs (architecture/radio.md, The QMX's settings).
// Anything else reads as none.
constexpr int kStationAmpPttNone = 0;
constexpr int kStationAmpPttGround = 1;  // TX PTT grounded: a Micro PA100+
constexpr int kStationAmpPtt5v = 2;      // TX PTT +5V

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
    int radio;  // kStationRadioQmx or kStationRadioHl2
    int gps_baud;
    std::string comment1;
    std::string active_bands;
    int tx_min_delay_ms = kStationTxMinDelayDefaultMs;  // 0..kStationTxMinDelayMaxMs
    bool amp_swr_guard = false;  // recover from the QMX's SWR-protection trips (architecture/radio.md)
    int amp_ptt = kStationAmpPttNone;  // kStationAmpPtt*: how the amp is keyed from the QMX's PTT jack
    bool amp_prime_on_band_change = false;  // brief QMX SWR Tune pulse after an explicit band change
    bool rx_ft4 = false;  // also decode FT4 beside FT8 from the QMX's I/Q, listen only (docs/architecture/decode.md, FT4)
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