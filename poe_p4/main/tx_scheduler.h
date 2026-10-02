#pragma once

#include <stdint.h>

#include "autoseq.h"

// B68: headless CAT tone TX scheduler for PoE-P4 (no UI).

void tx_scheduler_set_beacon(uint8_t mode);  // 0=OFF, 1=EVEN, 2=ODD
uint8_t tx_scheduler_beacon_mode(void);
// Slot parity the armed (or last armed) TX goes out on; SLOT_STATE's parity.
int tx_scheduler_target_parity(void);

void tx_scheduler_arm_from_autoseq_or_beacon(void);
void arm_pending_tx(const AutoseqTxEntry &pending);

// Call every main-loop tick: slot boundary + per-tone CAT TA.
void tx_scheduler_tick(void);

void tx_scheduler_cancel(void);

// Operate TX HUD: active while CAT tones are in flight; snapshot is the
// armed/in-flight message text + on-air offset (power/SWR filled by the caller).
bool tx_scheduler_hud_active(void);
bool tx_scheduler_hud_snapshot(const char **text_out, int *offset_hz_out);
