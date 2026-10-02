#pragma once

#include <string>

#include "adif.h"

// Live ADIF QSO archive write. Callers fill an explicit snapshot — no g_*
// globals. Dedupe object is owned by decode_pipeline (same ring used for
// "already worked").

struct AdifLogQsoInput {
    std::string dxcall;
    std::string dxgrid;
    int rst_sent = kAdifNoReport;
    int rst_rcvd = kAdifNoReport;
    std::string station_callsign;
    std::string my_gridsquare;
    std::string mode;     // e.g. "FT8"
    std::string freq_mhz; // e.g. "14.074"
    std::string band;     // ADIF tag value, e.g. "20M" -- already formatted by the caller
    std::string comment;  // already macro-expanded by the caller
    std::string my_pota_ref; // comma list, no program prefix, e.g. "US-1234,US-5678"
    std::string my_sota_ref; // comma list, no program prefix, e.g. "W7O/CC-001"
};

// Appends one record to YYYYMMDD.adi via storage_file_append.
// Returns true on success or intentional dedupe skip; false on write failure.
bool adif_log_qso(const AdifLogQsoInput& in, AdifLoggerDedupe* dedupe);

// The station-side values log_adif_entry() (below) needs, beyond the
// per-decode dxcall/dxgrid/rst_sent/rst_rcvd every caller already has to
// hand. The board fills this from its StationSettings; only the logic that turns these values into an AdifLogQsoInput is
// now shared, not the state itself.
struct AdifStationSnapshot {
    std::string call;
    std::string my_gridsquare;  // already truncated by the caller's own grid_ft8_4()
    std::string mode;           // e.g. "FT8"
    std::string freq_mhz;       // e.g. "14.074"
    std::string band_name;      // this app's own band name, e.g. "20m" -- formatted to
                                 // the ADIF tag value ("20M") inside adif_log_entry()
    std::string raw_comment;    // Station's comment1, not yet macro-expanded
    std::string radio_name;     // for the "/Radio" comment macro (radio_profile_name())
    std::string grid_for_macro; // for the "/Grid" comment macro (the station grid)
    std::string act_refs;       // Station's act_refs, e.g. "POTA:US-1234,SOTA:W7O/CC-001",
                                 // split into my_pota_ref/my_sota_ref inside adif_log_entry()
};

// Expands "/Radio"/"/Grid" in the comment
// and calls adif_log_qso() above. Returns the same as adif_log_qso().
bool adif_log_entry(const std::string& dxcall, const std::string& dxgrid, int rst_sent,
                    int rst_rcvd, const AdifStationSnapshot& station, AdifLoggerDedupe* dedupe);
