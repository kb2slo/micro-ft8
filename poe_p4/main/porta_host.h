#pragma once

// P4-only helpers owned by porta_host.cpp (station seed + Operate ticks).

void porta_p4_station_boot(void);
void porta_p4_adif_register(void);
void porta_p4_file_tick(void);
int porta_p4_station_freq_hz(void);
// Push the Station band to the VFO. nullptr on success, else why not.
const char *porta_p4_sync_vfo(const char *reason);
int porta_p4_station_offset_hz(void);
int porta_p4_station_offset_src(void);  // 0=RANDOM, 1=CURSOR, 2=RX
int porta_p4_station_tx_min_delay_ms(void);  // least TX;-to-first-RF gap, 0..500
void porta_p4_companion_status_tick(void);
void porta_p4_tx_hud_tick(void);
