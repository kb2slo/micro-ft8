#include "adif_logger.h"

#include <cstdio>
#include <ctime>

#include "board_power.h"
#include "esp_log.h"
#include "rtc_core.h"
#include "storage_service.h"

namespace {

const char* TAG = "adif_logger";

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
    fields.station_callsign = in.station_callsign;
    fields.my_gridsquare = in.my_gridsquare;
    fields.rst_sent = in.rst_sent;
    fields.rst_rcvd = in.rst_rcvd;
    fields.comment = in.comment;
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
