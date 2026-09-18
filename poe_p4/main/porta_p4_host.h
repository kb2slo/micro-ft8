#pragma once

// P4-only helpers owned by porta_host_stubs.cpp (station seed + Operate ticks).

void porta_p4_station_boot(void);
int porta_p4_station_freq_hz(void);
void porta_p4_companion_status_tick(void);
