#include "adif_logger.h"

#include <cstdio>
#include <ctime>

#include "board_power.h"
#include "esp_log.h"
#include "rtc_core.h"
#include "storage_service.h"

namespace {

const char* TAG = "adif_logger";

// The substitution both boards' comment macros need -- previously the ADV's
// own expand_comment_macros() (main/main.cpp), never ported to the P4. Takes
// already-resolved replacement values rather than reading globals, since the
// ADV's own grid-for-"/Grid" value can come from a GPS override the P4 has
// no equivalent of; each board resolves that one value itself and this just
// does the substitution.
std::string expand_comment_macros(const std::string& src, const std::string& radio_name,
                                  const std::string& grid_for_macro) {
    std::string out = src;
    auto repl = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    repl(out, "/Radio", radio_name);
    repl(out, "/Grid", grid_for_macro);
    return out;
}

// This app's own band names are lowercase, no unit ("20m"); POTA requires
// BAND with a unit, upper-case (RFC 0008 §3: "BAND (with unit, 20M)"). ADIF's
// own band enum is exactly the band name upper-cased plus the trailing unit
// letter this app's names already carry ("20m" -> "20M", "2m" -> "2M"), so
// this is a plain case fold, not a lookup table.
std::string format_adif_band_tag(const std::string& band_name) {
    std::string out = band_name;
    for (char& ch : out) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return out;
}

// station.act_refs is already normalized (station.cpp's normalize_act_refs():
// upper-cased, malformed tokens dropped) by the time it reaches here, so this
// only has to split it by program and drop each token's own "POTA:"/"SOTA:"
// prefix -- ADIF's MY_POTA_REF/MY_SOTA_REF are bare reference lists, the
// program is a separate field (MY_SIG) the exporter derives at n-fer
// expansion time, not stored per-QSO on the device.
void split_act_refs(const std::string& act_refs, std::string* pota_out, std::string* sota_out) {
    pota_out->clear();
    sota_out->clear();
    size_t start = 0;
    while (start <= act_refs.size()) {
        const size_t comma = act_refs.find(',', start);
        const std::string token =
            comma == std::string::npos ? act_refs.substr(start) : act_refs.substr(start, comma - start);
        const size_t colon = token.find(':');
        if (colon != std::string::npos) {
            const std::string program = token.substr(0, colon);
            const std::string ref = token.substr(colon + 1);
            std::string* bucket = (program == "SOTA") ? sota_out : (program == "POTA") ? pota_out : nullptr;
            if (bucket && !ref.empty()) {
                if (!bucket->empty()) {
                    *bucket += ",";
                }
                *bucket += ref;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
}

std::string normalize_call_token(std::string s) {
    if (!s.empty() && s.front() == '<') {
        s.erase(s.begin());
    }
    if (!s.empty() && s.back() == '>') {
        s.pop_back();
    }
    for (char& ch : s) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return s;
}

}  // namespace

bool adif_log_qso(const AdifLogQsoInput& in, AdifLoggerDedupe* dedupe) {
    if (board_power_writes_blocked()) {
        ESP_LOGW(TAG, "ADIF write skipped: writes blocked");
        return false;
    }
    if (!storage_service_firmware_available()) {
        ESP_LOGW(TAG, "ADIF write skipped: storage not ready");
        return false;
    }

    const int64_t now_ms = rtc_now_ms();
    const std::string call_norm = normalize_call_token(in.dxcall);
    if (dedupe && adif_logger_dedupe_is_duplicate(dedupe, call_norm, now_ms)) {
        ESP_LOGI(TAG, "ADIF dedupe skip %s (within 10 min)", call_norm.c_str());
        return true;
    }

    time_t now = static_cast<time_t>(now_ms / 1000);
    struct tm t {};
    localtime_r(&now, &t);
    char date[16];
    snprintf(date, sizeof(date), "%04d%02d%02d", (t.tm_year + 1900) % 10000,
             (t.tm_mon + 1) % 100, t.tm_mday % 100);
    char path[64];
    snprintf(path, sizeof(path), "%s.adi", date);

    char time_on[16];
    snprintf(time_on, sizeof(time_on), "%02d%02d%02d", t.tm_hour % 100, t.tm_min % 100,
             t.tm_sec % 100);

    AdifLogFields fields;
    fields.call = in.dxcall;
    fields.gridsquare = in.dxgrid;
    fields.mode = in.mode;
    fields.qso_date = date;
    fields.time_on = time_on;
    fields.freq = in.freq_mhz;
    fields.band = in.band;
    fields.station_callsign = in.station_callsign;
    fields.my_gridsquare = in.my_gridsquare;
    fields.rst_sent = in.rst_sent;
    fields.rst_rcvd = in.rst_rcvd;
    fields.comment = in.comment;
    fields.my_pota_ref = in.my_pota_ref;
    fields.my_sota_ref = in.my_sota_ref;
    const std::string line = adif_format_log_record(fields);

    if (!storage_file_append(path, line, "ADIF EXPORT\n<eoh>\n", true)) {
        ESP_LOGW(TAG, "ADIF write failed: %s", path);
        return false;
    }
    if (dedupe) {
        adif_logger_dedupe_remember(dedupe, call_norm, now_ms);
    }
    ESP_LOGI(TAG, "ADIF wrote %s -> %s", call_norm.c_str(), path);
    return true;
}

bool adif_log_entry(const std::string& dxcall, const std::string& dxgrid, int rst_sent,
                    int rst_rcvd, const AdifStationSnapshot& station, AdifLoggerDedupe* dedupe) {
    AdifLogQsoInput in;
    in.dxcall = dxcall;
    in.dxgrid = dxgrid;
    in.rst_sent = rst_sent;
    in.rst_rcvd = rst_rcvd;
    in.station_callsign = station.call;
    in.my_gridsquare = station.my_gridsquare;
    in.mode = station.mode;
    in.freq_mhz = station.freq_mhz;
    in.band = format_adif_band_tag(station.band_name);
    in.comment = expand_comment_macros(station.raw_comment, station.radio_name, station.grid_for_macro);
    split_act_refs(station.act_refs, &in.my_pota_ref, &in.my_sota_ref);
    return adif_log_qso(in, dedupe);
}
