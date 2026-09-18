#pragma once

#include <string>

#include "adif.h"

// Shared live ADIF QSO archive write (ADV + PoE-P4). Callers fill an explicit
// snapshot — no g_* globals. Dedupe object is owned by decode_pipeline on both
// boards (same ring used for "already worked" UI).

struct AdifLogQsoInput {
    std::string dxcall;
    std::string dxgrid;
    int rst_sent = kAdifNoReport;
    int rst_rcvd = kAdifNoReport;
    std::string station_callsign;
    std::string my_gridsquare;
    std::string mode;     // e.g. "FT8"
    std::string freq_mhz; // e.g. "14.074"
    std::string comment;  // already macro-expanded by the caller
};

// Appends one record to YYYYMMDD.adi via storage_file_append.
// Returns true on success or intentional dedupe skip; false on write failure.
bool adif_log_qso(const AdifLogQsoInput& in, AdifLoggerDedupe* dedupe);
