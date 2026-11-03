#pragma once

// The HL2 over the P4's Ethernet port, receive only (RFC 0014 S1, row mg9c; hl2_transport.cpp).

// Brings up the EMAC and IP101 with a link-local address, finds the HL2 by discovery, streams one receiver at
// 48 kHz on the station's dial, and logs the stream's health every 15 s. The samples go nowhere yet. Call once, at
// boot, only when Station's radio is the HL2; false if Ethernet could not start (logged).
bool hl2_transport_start(void);
