#pragma once

#include <cstdint>

#include "station_channel.h"

// P4-only helpers owned by porta_host.cpp (station seed + Operate ticks).

void porta_p4_station_boot(void);
// The decode pipeline could not start (no RAM for the waterfall): the radio is up and nothing
// decodes. Shown on the phone as decoding=0 in the config snapshot.
void porta_p4_set_decode_failed(bool failed);
void porta_p4_adif_register(void);
void porta_p4_file_tick(void);
int porta_p4_station_freq_hz(void);
// Push the Station band to the VFO. nullptr on success, else why not.
const char *porta_p4_sync_vfo(const char *reason);
int porta_p4_station_offset_hz(void);
int porta_p4_station_offset_src(void);  // 0=RANDOM, 1=CURSOR, 2=RX
bool porta_p4_station_call_ok(void);  // a callsign we may transmit under (tx_call_ok(), tx_ready.h)
int porta_p4_station_radio(void);  // Station's radio= (kStationRadioQmx, kStationRadioHl2)
bool porta_p4_tune_active(void);  // Tune is keying the radio (it never sets tx_active)
// The one stop control (Port A TX_CANCEL): the transmission, Tune, an amplifier prime and an SWR sweep. Main loop.
void porta_host_tx_cancel(void);
bool porta_p4_station_amp_swr_guard(void);  // handle the QMX's SWR-protection trips
// The operating band's plan: the radio's dial, the channels heard there and FT4's dial beside FT8's
// (station_plan(), station_channel.h). Main loop.
StationPlan porta_p4_station_plan(void);
// FT4's dial minus the radio's while FT4 is heard beside FT8, in Hz; 0 otherwise, FT4 alone included (its dial
// is the radio's). Main loop.
int32_t porta_p4_station_ft4_dial_offset_hz(void);
// The station's dials now: the FT8 dial set for the band, the one the radio reports, and FT4's beside it
// (station_channel.h). Main loop.
StationDials porta_p4_station_dials(void);
// A transmission is due: prime the amplifier first when its band was never primed (set at boot or while CAT
// was down; docs/architecture/radio.md, Prime an RF-sensing amplifier). True while that prime runs.
bool porta_p4_amp_prime_before_tx(void);
// The band last primed or tried, AMP_BAND_PRIME_NONE (-1) before any: for a trip's log line.
int porta_p4_amp_primed_band(void);
int porta_p4_station_tx_min_delay_ms(void);  // least TX;-to-first-RF gap, 0..500
void porta_p4_companion_status_tick(void);
void porta_p4_tx_hud_tick(void);
void porta_p4_radio_push_tick(void);
