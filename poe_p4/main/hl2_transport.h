#pragma once

// The HL2 over the P4's Ethernet port, receive only (RFC 0014 S1, row mg9c; hl2_transport.cpp).

// Active: finds the HL2 by discovery, streams one receiver at 48 kHz on the station's dial, and logs the stream's
// health every 15 s; the samples go nowhere yet. The first activation brings up the EMAC and IP101 with a
// link-local address, which then stay up. Inactive: sends stop and holds the stream off. Main loop only. False if
// Ethernet could not start (logged); it is not retried until the P4 restarts.
bool hl2_transport_set_active(bool active);
