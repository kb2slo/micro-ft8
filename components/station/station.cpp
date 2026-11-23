#include "station.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string trim_copy(const std::string& s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

void ascii_upper_inplace(std::string& s)
{
    for (char& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
}

std::string trim_upper_copy(const std::string& s)
{
    std::string out = trim_copy(s);
    ascii_upper_inplace(out);
    return out;
}

std::string normalize_grid_maidenhead(const std::string& src)
{
    size_t b = 0;
    size_t e = src.size();
    while (b < e && std::isspace(static_cast<unsigned char>(src[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(src[e - 1]))) {
        --e;
    }

    const size_t n = e - b;
    if (n != 4 && n != 6 && n != 8) {
        return "";
    }

    std::string out = src.substr(b, n);
    auto is_digit_char = [](char ch) { return ch >= '0' && ch <= '9'; };
    auto to_upper = [](char ch) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    };
    auto to_lower = [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    };

    char c0 = to_upper(out[0]);
    char c1 = to_upper(out[1]);
    if (c0 < 'A' || c0 > 'R' || c1 < 'A' || c1 > 'R') {
        return "";
    }
    if (!is_digit_char(out[2]) || !is_digit_char(out[3])) {
        return "";
    }
    out[0] = c0;
    out[1] = c1;

    if (n >= 6) {
        char c4 = to_upper(out[4]);
        char c5 = to_upper(out[5]);
        if (c4 < 'A' || c4 > 'X' || c5 < 'A' || c5 > 'X') {
            return "";
        }
        out[4] = to_lower(c4);
        out[5] = to_lower(c5);
    }

    if (n == 8) {
        if (!is_digit_char(out[6]) || !is_digit_char(out[7])) {
            return "";
        }
    }

    return out;
}

// "POTA:US-1234,SOTA:W7O/CC-001" (architecture/webapp.md, "Activation").
// Unlike normalize_grid_maidenhead() above (one value, all-or-nothing), this is a
// list -- a malformed token is dropped on its own rather than rejecting the
// whole write, so one bad entry can't also cost the good ones. A token
// needs a non-empty program and a non-empty reference either side of the
// first ':'; the program is upper-cased to POTA/SOTA and kept only if it
// matches one of those two, the reference is upper-cased as-is (both
// programs' own reference formats are already upper-case by convention).
// Empty input is a valid value (no activation), not a failure.
std::string normalize_act_refs(const std::string& src)
{
    const std::string trimmed = trim_copy(src);
    if (trimmed.empty()) {
        return "";
    }
    std::string out;
    size_t start = 0;
    while (start <= trimmed.size()) {
        const size_t comma = trimmed.find(',', start);
        const std::string token = trim_copy(
            comma == std::string::npos ? trimmed.substr(start) : trimmed.substr(start, comma - start));
        const size_t colon = token.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < token.size()) {
            std::string program = token.substr(0, colon);
            std::string ref = token.substr(colon + 1);
            ascii_upper_inplace(program);
            ascii_upper_inplace(ref);
            if (program == "POTA" || program == "SOTA") {
                if (!out.empty()) {
                    out += ",";
                }
                out += program + ":" + ref;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

int normalize_gps_baud(int value)
{
    return (value == 9600 || value == 115200) ? value : 115200;
}

int parse_radio_value(const char* raw)
{
    if (!raw) {
        return kStationRadioQmx;
    }

    char* end = nullptr;
    long as_int = strtol(raw, &end, 10);
    if (end != raw) {
        switch (static_cast<int>(as_int)) {
            case kStationRadioHl2:
                return kStationRadioHl2;
            case kStationRadioQmx:
            default:
                return kStationRadioQmx;
        }
    }

    std::string token;
    for (const char* p = raw; *p; ++p) {
        unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            continue;
        }
        token.push_back(static_cast<char>(std::toupper(ch)));
    }

    if (token == "HL2") {
        return kStationRadioHl2;
    }
    return kStationRadioQmx;
}

void format_freq(float f, char* fbuf, size_t n)
{
    if (f == static_cast<int>(f)) {
        snprintf(fbuf, n, "%d", static_cast<int>(f));
    } else {
        snprintf(fbuf, n, "%.1f", f);
    }
}

enum class StationKey {
    Offset,
    BandSel,
    CqMod,
    OffsetSrc,
    Radio,
    GpsBaud,
    Call,
    Grid,
    Comment1,
    Amp,
    AmpSwrGuard,
    AmpPtt,
    AmpPrimeOnBandChange,
    SkipTx1,
    ActiveBand,
    ActiveBands,
    Tries,
    TxMinDelay,
    ActRefs,
    QrzKey,
    QrzBook,
    QrzLive,
    DistUnit,
    CqMode,
    ShowBearing,
    QueueMarks,
    PskrReport,
    Unknown,
};

StationKey key_from_name(const std::string& name)
{
    if (name == "offset") return StationKey::Offset;
    if (name == "band_sel") return StationKey::BandSel;
    if (name == "cq_mod") return StationKey::CqMod;
    if (name == "offset_src") return StationKey::OffsetSrc;
    if (name == "radio") return StationKey::Radio;
    if (name == "gps_baud") return StationKey::GpsBaud;
    if (name == "call") return StationKey::Call;
    if (name == "grid") return StationKey::Grid;
    if (name == "comment1") return StationKey::Comment1;
    if (name == "amp") return StationKey::Amp;
    if (name == "amp_swr_guard") return StationKey::AmpSwrGuard;
    if (name == "amp_ptt") return StationKey::AmpPtt;
    if (name == "amp_prime_on_band_change") return StationKey::AmpPrimeOnBandChange;
    if (name == "skiptx1") return StationKey::SkipTx1;
    if (name == "active_band") return StationKey::ActiveBand;
    if (name == "active_bands") return StationKey::ActiveBands;
    if (name == "tries") return StationKey::Tries;
    if (name == "tx_min_delay_ms") return StationKey::TxMinDelay;
    if (name == "act_refs") return StationKey::ActRefs;
    if (name == "qrz_key") return StationKey::QrzKey;
    if (name == "qrz_book") return StationKey::QrzBook;
    if (name == "qrz_live") return StationKey::QrzLive;
    if (name == "dist_unit") return StationKey::DistUnit;
    if (name == "cq_mode") return StationKey::CqMode;
    if (name == "show_bearing") return StationKey::ShowBearing;
    if (name == "queue_marks") return StationKey::QueueMarks;
    if (name == "pskr_report") return StationKey::PskrReport;
    return StationKey::Unknown;
}

bool all_chars(const std::string& s, const char* extra)
{
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && !strchr(extra, c)) {
            return false;
        }
    }
    return true;
}

// The checks station_pref_refusal() reports and apply_key() keeps the old value on: the value as
// it is stored (trimmed), or why it is not one.
const char* pref_check(StationKey key, const std::string& v)
{
    switch (key) {
        case StationKey::QrzKey:
            return v.size() <= 64 && all_chars(v, "-") ? nullptr : "a QRZ key is letters, digits and dashes";
        case StationKey::QrzBook:
            return v.size() <= 16 && all_chars(v, "/") ? nullptr : "a QRZ logbook is a callsign";
        case StationKey::QrzLive: {
            bool day = v.size() == 8;
            for (char c : v) day = day && c >= '0' && c <= '9';
            return v.empty() || day ? nullptr : "the QRZ sending day is YYYYMMDD";
        }
        case StationKey::DistUnit:
            return v == "km" || v == "mi" ? nullptr : "units are km or mi";
        case StationKey::CqMode:
            return v == "FT8" || v == "FT4" ? nullptr : "the CQ mode is FT8 or FT4";
        case StationKey::ShowBearing:
        case StationKey::QueueMarks:
        case StationKey::PskrReport:
            return v == "0" || v == "1" ? nullptr : "the switch is 0 or 1";
        default:
            return nullptr;
    }
}

void apply_key(StationSettings* io, StationKey key, const std::string& value)
{
    int val = 0;
    switch (key) {
        case StationKey::Offset:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->offset_hz = val;
            }
            break;
        case StationKey::BandSel:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                if (val >= 0 && val < kStationBandCount) {
                    io->band_sel = val;
                }
            }
            break;
        case StationKey::CqMod: {
            // A bad one keeps the old, as a bad grid does; the main board
            // refuses it before it gets here (porta_host_config_set()).
            const std::string mod = trim_upper_copy(value);
            if (station_cq_mod_ok(mod)) {
                io->cq_mod = mod;
            }
            break;
        }
        case StationKey::OffsetSrc:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                if (val >= 0 && val <= 2) {
                    io->offset_src = val;
                }
            }
            break;
        case StationKey::Radio:
            io->radio = parse_radio_value(value.c_str());
            break;
        case StationKey::GpsBaud:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->gps_baud = normalize_gps_baud(val);
            }
            break;
        case StationKey::Call:
            io->call = trim_upper_copy(value);
            break;
        case StationKey::Grid: {
            const std::string norm = normalize_grid_maidenhead(value);
            if (!norm.empty()) {
                io->grid = norm;
            }
            break;
        }
        case StationKey::Comment1:
            io->comment1 = trim_copy(value);
            break;
        case StationKey::Amp:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->amp = (val != 0);
            }
            break;
        case StationKey::AmpSwrGuard:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->amp_swr_guard = (val != 0);
            }
            break;
        case StationKey::AmpPtt:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->amp_ptt = (val == kStationAmpPttGround || val == kStationAmpPtt5v) ? val : kStationAmpPttNone;
            }
            break;
        case StationKey::AmpPrimeOnBandChange:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->amp_prime_on_band_change = (val != 0);
            }
            break;
        case StationKey::SkipTx1:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->skip_tx1 = (val != 0);
            }
            break;
        case StationKey::ActiveBand:
            if (sscanf(value.c_str(), "%d", &val) == 1) {
                io->active_bands = std::to_string(val);
            }
            break;
        case StationKey::ActiveBands:
            io->active_bands = trim_upper_copy(value);
            break;
        case StationKey::Tries:
            if (sscanf(value.c_str(), "%d", &val) == 1 && val >= 1) {
                io->tries = val;
            }
            break;
        case StationKey::TxMinDelay:
            if (sscanf(value.c_str(), "%d", &val) == 1 && val >= 0 &&
                val <= kStationTxMinDelayMaxMs) {
                io->tx_min_delay_ms = val;
            }
            break;
        case StationKey::ActRefs:
            io->act_refs = normalize_act_refs(value);
            break;
        case StationKey::QrzKey:
        case StationKey::QrzBook:
        case StationKey::QrzLive:
        case StationKey::DistUnit:
        case StationKey::CqMode:
        case StationKey::ShowBearing:
        case StationKey::QueueMarks:
        case StationKey::PskrReport: {
            // A bad value keeps the old; the main board refuses it before it gets here
            // (station_pref_refusal()).
            const std::string v = trim_copy(value);
            if (pref_check(key, v)) {
                break;
            }
            if (key == StationKey::QrzKey) io->qrz_key = v;
            if (key == StationKey::QrzBook) io->qrz_book = trim_upper_copy(v);
            if (key == StationKey::QrzLive) io->qrz_live = v;
            if (key == StationKey::DistUnit) io->dist_unit = v;
            if (key == StationKey::CqMode) io->cq_mode = v;
            if (key == StationKey::ShowBearing) io->show_bearing = (v == "1");
            if (key == StationKey::QueueMarks) io->queue_marks = (v == "1");
            if (key == StationKey::PskrReport) io->pskr_report = (v == "1");
            break;
        }
        case StationKey::Unknown:
            break;
    }
}

void apply_line(StationSettings* io, const std::string& raw)
{
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.empty()) {
        return;
    }

    int idx = -1;
    float fval = 0.0f;
    int ival = 0;
    if (sscanf(line.c_str(), "listen%d=%d", &idx, &ival) == 2) {
        if (idx >= 0 && idx < kStationBandCount) {
            io->listen[idx] = (ival >= kStationListenFt8 && ival <= kStationListenBoth) ? ival : kStationListenFt8;
        }
        return;
    }
    if (sscanf(line.c_str(), "ft4band%d=%f", &idx, &fval) == 2) {
        if (idx >= 0 && idx < kStationBandCount) {
            io->ft4_freq[idx] = fval;
            io->ft4_freq_set[idx] = true;
        }
        return;
    }
    if (sscanf(line.c_str(), "band%d=%f", &idx, &fval) == 2) {
        if (idx >= 0 && idx < kStationBandCount) {
            io->band_freq[idx] = fval;
            io->band_freq_set[idx] = true;
        }
        return;
    }

    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
        return;
    }
    apply_key(io, key_from_name(line.substr(0, eq)), line.substr(eq + 1));
}

// key=value lines for station_serialize(). Plain appends, not an ostringstream: a stream links
// the C++ locale machinery into the P4 image (docs/architecture/memory.md).
void put_str(std::string& out, const char* key, const std::string& value)
{
    out += key;
    out += '=';
    out += value;
    out += '\n';
}

void put_int(std::string& out, const char* key, int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    put_str(out, key, buf);
}

}  // namespace

void station_settings_init(StationSettings* out)
{
    if (!out) {
        return;
    }
    *out = StationSettings{};
    out->gps_baud = 115200;
    out->radio = kStationRadioQmx;
    for (int i = 0; i < kStationBandCount; ++i) {
        out->listen[i] = kStationListenFt8;
    }
}

int station_listen(const StationSettings& s, int band)
{
    if (band < 0 || band >= kStationBandCount) {
        return kStationListenFt8;
    }
    const int v = s.listen[band];
    return (v >= kStationListenFt8 && v <= kStationListenBoth) ? v : kStationListenFt8;
}

void station_parse(const std::string& text, StationSettings* io)
{
    if (!io) {
        return;
    }
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            apply_line(io, text.substr(start));
            break;
        }
        apply_line(io, text.substr(start, end - start));
        start = end + 1;
    }
}

std::string station_serialize(const StationSettings& in)
{
    std::string out;
    for (int i = 0; i < kStationBandCount; ++i) {
        if (!in.band_freq_set[i]) {
            continue;
        }
        char key[16];
        snprintf(key, sizeof(key), "band%d", i);
        char fbuf[16];
        format_freq(in.band_freq[i], fbuf, sizeof(fbuf));
        put_str(out, key, fbuf);
    }
    for (int i = 0; i < kStationBandCount; ++i) {
        if (!in.ft4_freq_set[i]) {
            continue;
        }
        char key[16];
        snprintf(key, sizeof(key), "ft4band%d", i);
        char fbuf[16];
        format_freq(in.ft4_freq[i], fbuf, sizeof(fbuf));
        put_str(out, key, fbuf);
    }
    for (int i = 0; i < kStationBandCount; ++i) {
        if (station_listen(in, i) == kStationListenFt8) {
            continue;  // the default, left unwritten as an unset band frequency is
        }
        char key[16];
        snprintf(key, sizeof(key), "listen%d", i);
        put_int(out, key, station_listen(in, i));
    }
    put_int(out, "offset", in.offset_hz);
    put_int(out, "band_sel", in.band_sel);
    put_str(out, "cq_mod", in.cq_mod);
    put_int(out, "skiptx1", in.skip_tx1 ? 1 : 0);
    put_str(out, "call", in.call);
    put_str(out, "grid", in.grid);
    put_int(out, "offset_src", in.offset_src);
    put_int(out, "radio", in.radio);
    put_int(out, "gps_baud", normalize_gps_baud(in.gps_baud));
    put_str(out, "comment1", in.comment1);
    put_str(out, "active_bands", in.active_bands);
    put_int(out, "tries", in.tries);
    put_int(out, "amp", in.amp ? 1 : 0);
    put_int(out, "tx_min_delay_ms", in.tx_min_delay_ms);
    put_int(out, "amp_swr_guard", in.amp_swr_guard ? 1 : 0);
    put_int(out, "amp_ptt", in.amp_ptt);
    put_int(out, "amp_prime_on_band_change", in.amp_prime_on_band_change ? 1 : 0);
    put_str(out, "act_refs", in.act_refs);
    put_str(out, "qrz_key", in.qrz_key);
    put_str(out, "qrz_book", in.qrz_book);
    put_str(out, "qrz_live", in.qrz_live);
    put_str(out, "dist_unit", in.dist_unit);
    put_str(out, "cq_mode", in.cq_mode);
    put_int(out, "show_bearing", in.show_bearing ? 1 : 0);
    put_int(out, "queue_marks", in.queue_marks ? 1 : 0);
    put_int(out, "pskr_report", in.pskr_report ? 1 : 0);
    return out;
}

const char* station_pref_refusal(const char* key, const std::string& value)
{
    if (!key) {
        return nullptr;
    }
    return pref_check(key_from_name(key), trim_copy(value));
}

bool station_key_secret(const char* name)
{
    return name && strcmp(name, "qrz_key") == 0;
}

std::string station_mask_secret(const std::string& value)
{
    std::string out(value.size(), 'x');
    if (value.size() > 8) {
        out.replace(value.size() - 4, 4, value.substr(value.size() - 4));
    }
    return out;
}

StationSettings station_masked(const StationSettings& in)
{
    StationSettings out = in;
    out.qrz_key = station_mask_secret(in.qrz_key);
    return out;
}

int station_tx_min_delay_ms(const StationSettings& s)
{
    return s.amp ? s.tx_min_delay_ms : kStationTxMinDelayDefaultMs;
}

bool station_amp_swr_guard(const StationSettings& s)
{
    return s.amp && s.amp_swr_guard;
}

bool station_amp_prime_on_band_change(const StationSettings& s)
{
    return s.amp && s.amp_prime_on_band_change;
}

bool station_cq_mod_ok(const std::string& mod)
{
    if (mod.empty()) return true;
    if (mod.size() > 4) return false;
    bool letters = true;
    bool digits = true;
    for (char c : mod) {
        if (c < 'A' || c > 'Z') letters = false;
        if (c < '0' || c > '9') digits = false;
    }
    return letters || (digits && mod.size() == 3);
}

bool act_refs_prefer_sota(const std::string& act_refs)
{
    return act_refs.find("SOTA:") != std::string::npos;
}

bool station_key_known(const char* name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    StationKey key = key_from_name(name);
    switch (key) {
        case StationKey::Unknown:
            break;
        default:
            return true;
    }
    int idx = 0;
    if (sscanf(name, "band%d", &idx) == 1 || sscanf(name, "ft4band%d", &idx) == 1 ||
        sscanf(name, "listen%d", &idx) == 1) {
        return idx >= 0 && idx < kStationBandCount;
    }
    return false;
}

bool station_key_names_operating_band(const char* key, int band_sel)
{
    if (!key) {
        return false;
    }
    if (strcmp(key, "band_sel") == 0) {
        return true;
    }
    int idx = -1;
    return (sscanf(key, "band%d", &idx) == 1 || sscanf(key, "ft4band%d", &idx) == 1 ||
            sscanf(key, "listen%d", &idx) == 1) && idx == band_sel;
}

const char* station_vfo_refusal(const char* key, bool tx_active)
{
    const bool band = key && strcmp(key, "band_sel") == 0;
    if (band) {
        return tx_active ? "band saved, VFO not moved (TX active)"
                         : "band saved, VFO not moved (CAT not ready)";
    }
    return tx_active ? "frequency saved, VFO not moved (TX active)"
                     : "frequency saved, VFO not moved (CAT not ready)";
}
