#pragma once

#include <cstdint>

// The HL2 over the P4's Ethernet port, receive only (RFC 0014 S1, row mg9c; hl2_transport.cpp).

// Active: finds the HL2 by discovery, streams one receiver at 48 kHz on the station's dial, and logs the stream's
// health every 15 s; its I/Q feeds decode (hl2_audio_*, below). The first activation brings up the EMAC and IP101 with a
// link-local address, which then stay up. Inactive: sends stop and holds the stream off. Main loop only. False if
// Ethernet could not start (logged); it is not retried until the P4 restarts.
bool hl2_transport_set_active(bool active);

// The station's dial, in hertz: the receiver's NCO follows it, retuning a live stream, and the dial history notes
// it so each decode carries it. Main loop only (porta_host.cpp's sync_vfo, radio_select.cpp).
void hl2_transport_set_dial(uint32_t hz);

// ft8_audio_pipeline_config_t's callbacks for the HL2: 6 kHz audio from its I/Q through components/iq_frontend.
// Ready while a stream is arriving; read and the front end run on the pipeline's ingest task.
bool hl2_audio_ready(void);
bool hl2_audio_should_stop(void* ctx);
int hl2_audio_read_ft8_samples(void* ctx, float* out, int max_samples);
uint32_t hl2_audio_take_frontend_us(void* ctx);
int hl2_audio_rx_path_for_slot(void* ctx, int64_t slot_idx);
