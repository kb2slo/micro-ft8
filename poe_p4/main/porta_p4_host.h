#pragma once

// P4-only helpers owned by porta_host_stubs.cpp (station seed + Operate ticks).

void porta_p4_station_boot(void);
int porta_p4_station_freq_hz(void);
int porta_p4_station_offset_hz(void);
int porta_p4_station_offset_src(void);  // 0=RANDOM, 1=CURSOR, 2=RX
void porta_p4_companion_status_tick(void);
void porta_p4_tx_hud_tick(void);
