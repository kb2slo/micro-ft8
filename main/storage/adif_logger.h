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

// The station-side values log_adif_entry() (below) needs, beyond the
// per-decode dxcall/dxgrid/rst_sent/rst_rcvd every caller already has to
// hand. Each board fills this from its own state (loose globals on the ADV,
// a persistent StationSettings on the P4 -- row j4xh) the same way it always
// has; only the logic that turns these values into an AdifLogQsoInput is
// now shared, not the state itself.
struct AdifStationSnapshot {
    std::string call;
    std::string my_gridsquare;  // already truncated by the caller's own grid_ft8_4()
    std::string mode;           // e.g. "FT8"
    std::string freq_mhz;       // e.g. "14.074"
    std::string raw_comment;    // Station's comment1, not yet macro-expanded
    std::string radio_name;     // for the "/Radio" comment macro (radio_profile_name())
    std::string grid_for_macro; // for the "/Grid" comment macro (the ADV's GPS override
                                 // wins here when it applies; the P4 has no GPS, so this
                                 // is just its own grid)
};

// Replaces both boards' separate log_adif_entry() (main/main.cpp,
// poe_p4/main/porta_host_stubs.cpp): expands "/Radio"/"/Grid" in the comment
// (previously ADV-only -- the P4 skipped it entirely, a confirmed drift) and
// calls adif_log_qso() above. Returns the same as adif_log_qso().
bool adif_log_entry(const std::string& dxcall, const std::string& dxgrid, int rst_sent,
                    int rst_rcvd, const AdifStationSnapshot& station, AdifLoggerDedupe* dedupe);
