#pragma once

#include "json_parser.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// One logged QSO, from the ADIF callback. The live server turns these into
// /api/log/entries rows.
struct SimLogEntry {
    std::string time_on;
    std::string call;
    std::string grid;
    int rst_sent = 0;
    int rst_rcvd = 0;
    std::string station_call;
    std::string my_grid;
};

// The period stepper host_test and sim_station share. One scenario at a time:
// the ADIF callback is a process-wide hook.
struct Scenario {
    TestData data;
    int period = 0;
    int beacon = 0;             // 0 off, 1 even, 2 odd
    int my_tx_parity = 0;
    bool check_expect = true;
    bool wall_slots = false;    // live: stamp si from the wall clock
    uint32_t dial_hz = 14074000;
    uint32_t next_seq = 1;
    uint32_t next_decode_id = 1;
    uint32_t epoch_secs = 0;
    uint32_t slot_idx = 0;
    uint8_t last_parity = 0;
    uint16_t last_offset = 0;
    bool hud_active = false;
    std::string hud_text;
    uint16_t hud_hz = 0;
    std::string station_call;
    std::string station_grid;
    std::string period_log;
    std::string period_cabrillo;
    std::vector<std::string> events;   // JSON objects, no leading comma
    std::map<uint32_t, UiRxLine> decodes;
    std::vector<uint16_t> last_ids;
    std::vector<SimLogEntry> log;
    int failures = 0;
};

void scenario_begin(Scenario& s, const TestData& data);

// One JSON period. False when the script is finished.
bool scenario_step(Scenario& s);

// Slot state plus the active queue, and an IDLE row for any entry that left.
// The live server calls this about once a second so the page's queue does
// not go stale between periods.
void scenario_emit_status(Scenario& s);

// Repeat the active TX HUD. The page hides it after three seconds of silence.
void scenario_emit_hud(Scenario& s);
void scenario_clear_hud(Scenario& s);

void scenario_emit_usb(Scenario& s, bool present);

// One SWR sweep event row (the same JSON the sidekick serves; RFC 0010).
void scenario_emit_sweep(Scenario& s, uint8_t kind, uint8_t range_index, uint32_t freq_hz, int swr_c,
                         uint8_t reason);

void scenario_set_beacon(Scenario& s, int mode);
void scenario_set_station(Scenario& s, const std::string& call, const std::string& grid);
void scenario_set_cq_type(Scenario& s, int type, const std::string& free_text);
void scenario_set_max_retry(Scenario& s, int retry);
bool scenario_reply(Scenario& s, uint32_t decode_id);
bool scenario_cancel_entry(Scenario& s, uint16_t entry_id);
void scenario_cancel_tx(Scenario& s);
