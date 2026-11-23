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

// Station.txt's listen<N>= values: the channels the station listens to on band N, as a set of modes
// (docs/architecture/decode.md, Channels). Each channel's frequency is the band's dial for that mode, band<N>
// and ft4band<N>. Unset, or anything else, reads as FT8 alone.
constexpr int kStationListenFt8 = 1;
constexpr int kStationListenFt4 = 2;
constexpr int kStationListenBoth = kStationListenFt8 | kStationListenFt4;

struct StationSettings {
    float band_freq[kStationBandCount];
    bool band_freq_set[kStationBandCount];
    // Band N's FT4 dial in kHz, ft4band<N>=; unset, the band's standard FT4 dial (docs/architecture/decode.md, Channels).
    float ft4_freq[kStationBandCount];
    bool ft4_freq_set[kStationBandCount];

    int offset_hz;
    int band_sel;
    std::string cq_mod;  // the CQ's modifier, "" for a plain CQ (station_cq_mod_ok())
    bool skip_tx1;
    std::string call;
    std::string grid;
    int offset_src;
    int radio;  // kStationRadioQmx or kStationRadioHl2
    int gps_baud;
    std::string comment1;
    std::string active_bands;
    // An amplifier is in use. Off, the four settings below are kept but not in force: read them
    // through station_tx_min_delay_ms(), station_amp_swr_guard() and station_amp_prime_on_band_change().
    bool amp = false;
    int tx_min_delay_ms = kStationTxMinDelayDefaultMs;  // 0..kStationTxMinDelayMaxMs
    bool amp_swr_guard = false;  // recover from the QMX's SWR-protection trips (architecture/radio.md)
    int amp_ptt = kStationAmpPttNone;  // kStationAmpPtt*: how the amp is keyed from the QMX's PTT jack
    bool amp_prime_on_band_change = false;  // brief QMX SWR Tune pulse after an explicit band change
    int listen[kStationBandCount];  // kStationListen* per band: which channels it listens to
    int tries;  // transmissions per QSO message, and a CQ's to start (autoseq.md, Tries)
    // "POTA:US-1234,SOTA:W7O/CC-001" -- the active activation's references,
    // entered in Settings > Station (architecture/webapp.md, "Activation").
    // Empty means no activation. Normalized (program upper-cased, malformed
    // tokens dropped) by station_parse(); see normalize_act_refs() in
    // station.cpp.
    std::string act_refs;

    // Kept for the web app, which acts on them; the main board stores and serves them so every
    // phone, at every address, sees one station (docs/architecture/webapp.md, Settings that live on the station).
    std::string qrz_key;   // the QRZ logbook's API key, "" for none; an open read masks it (station_mask_secret())
    std::string qrz_book;  // the logbook's callsign, once QRZ has said, "" before
    std::string qrz_live;  // the UTC day (YYYYMMDD) sending to QRZ by itself was turned on, "" for off
    std::string dist_unit = "km";  // "km" or "mi"
    std::string cq_mode = "FT8";   // the mode the CQ row calls in, "FT8" or "FT4"
    bool show_bearing = false;     // decode rows show the bearing beside the distance
    bool queue_marks = true;       // decodes about a queued station are marked and sorted next
    bool pskr_report = false;      // the web app reports what the station hears to PSKReporter
};

void station_settings_init(StationSettings* out);

// The amplifier settings in force (architecture/radio.md, The amplifier switch). With amp off: the
// default TX delay, no recovery from SWR-protection trips and no prime on a band change, whatever
// is saved, so one switch stops everything an amplifier adds, the prime's carrier included.
int station_tx_min_delay_ms(const StationSettings& s);
bool station_amp_swr_guard(const StationSettings& s);
bool station_amp_prime_on_band_change(const StationSettings& s);

// Overlay key=value lines onto *io. Unknown keys ignored. date=, time=,
// rtc_comp=, and rtc_sleep_epoch= are ignored (clock is not persisted).
void station_parse(const std::string& text, StationSettings* io);

// Writes the same keys save_station_data() does.
std::string station_serialize(const StationSettings& in);

// The channels band `band` listens to (kStationListen*): FT8 alone unless set.
int station_listen(const StationSettings& s, int band);

// True for Station.txt keys CONFIG may set (call, grid, band0…, ft4band0…, listen0…, …). Clock
// lines are not included — the clock is not persisted here.
bool station_key_known(const char* name);

// The refusal for a CONFIG write of one of the web app's settings (qrz_key, qrz_book, qrz_live,
// dist_unit, cq_mode, show_bearing, queue_marks, pskr_report) whose value station_parse() would not take, or
// nullptr. nullptr too for any other key: those are checked where they are used.
const char* station_pref_refusal(const char* key, const std::string& value);

// True for a key whose value an open read must not carry whole: qrz_key.
bool station_key_secret(const char* name);

// A secret as an open read shows it: every character but the last 4 as 'x', or every one when it
// is 8 characters or fewer, so a short value is not most of the way given away. "" stays "".
std::string station_mask_secret(const std::string& value);

// The settings with each secret masked, for the config read that is open to anyone on the network.
StationSettings station_masked(const StationSettings& in);

// A CQ modifier a standard message can carry: "" (a plain CQ), one to four
// letters A-Z ("DX", "POTA", "HI"), or three digits ("123"). `mod` is already
// trimmed and upper-cased. The web app's cqModOk() is held to this by the
// same vectors (webapp/tests/test_cq_mod.js, host_mock/test_station.cpp).
bool station_cq_mod_ok(const std::string& mod);

// On activation start (act_refs empty -> non-empty), the CQ's modifier is derived
// from the references themselves -- SOTA if any SOTA reference is present
// (a mixed activation defaults to SOTA), else POTA (architecture/logging.md,
// "The CQ's modifier follows the activation"). act_refs is already
// normalize_act_refs()'d ("POTA:"/"SOTA:" upper-case prefixes) by the time
// a caller has it, on either board.
bool act_refs_prefer_sota(const std::string& act_refs);

// True when a CONFIG write of this key needs the VFO already on the
// operating band to take effect right now: band_sel itself (it picks the new
// operating band), that band's own frequency keys, band<band_sel> and
// ft4band<band_sel>, or its channels, listen<band_sel>, which can move the dial to FT4's. A
// frequency written for a different band is a config edit that does not need
// the radio to move.
bool station_key_names_operating_band(const char* key, int band_sel);

// The CONFIG refusal for such a write when the VFO did not move: "band saved,
// VFO not moved (…)" for band_sel, "frequency saved, VFO not moved (…)" for a
// frequency key, with "TX active" or "CAT not ready" as the reason.
const char* station_vfo_refusal(const char* key, bool tx_active);