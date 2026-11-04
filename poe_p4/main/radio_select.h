#pragma once

// The chosen radio (radio_select.cpp): it feeds channel 0, decode's only channel today, and it is the radio the
// station's controls act on through radio_control (transmit, Tune, sweeps, band changes). A reply goes out on the
// radio that heard it, never another (RFC 0014 §8, 2026-11-03). One radio is chosen at a time for now.

// Makes Station's radio value (kStationRadioQmx, kStationRadioHl2) the active radio, at once and with no restart
// (RFC 0014 §8, 2026-11-03). Main loop only, and only while nothing is keyed: porta_host_config_set() refuses the
// change while a transmission, Tune, a sweep or an amplifier prime runs. Called at boot too.
void radio_select_apply(int station_radio);

// True while the QMX is the active radio, so its audio may feed decode. Any task.
bool radio_select_qmx_feeds_decode(void);

// True while the HL2 is the active radio, so its I/Q feeds decode. Any task.
bool radio_select_hl2_feeds_decode(void);

// The active radio's receive stream is up: the QMX's UAC, or the HL2's stream. Any task.
bool radio_select_audio_ready(void);
