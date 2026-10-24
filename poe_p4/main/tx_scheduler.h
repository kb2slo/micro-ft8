#pragma once

#include <stdint.h>

#include "autoseq.h"

// B68: headless CAT tone TX scheduler for PoE-P4 (no UI).

// Slot parity the armed (or last armed) TX goes out on; SLOT_STATE's parity.
int tx_scheduler_target_parity(void);

void tx_scheduler_arm_from_autoseq(void);
void arm_pending_tx(const AutoseqTxEntry &pending);

// Re-arm from the queue after the operator changed it (Call CQ, the CQ's
// parity, an entry's tries). Leaves a TX on the air alone.
void tx_scheduler_rearm(void);

// Call CQ on the slot it can still key, or the next (autoseq_cq_parity()).
// False if a CQ is already queued or the queue is full.
bool tx_scheduler_call_cq(void);

// Call every main-loop tick: slot boundary + per-tone CAT TA.
void tx_scheduler_tick(void);

void tx_scheduler_cancel(void);

// SWR protection (docs/architecture/radio.md, SWR protection trips on a QMX). The operator
// called CQ, replied or pressed Tune: leave the stopped state, clearing the radio's lock when
// it was set (`clear_anyway`: Tune, since its own trip leaves the lock set).
void tx_scheduler_swr_operator_start(bool clear_anyway);
// The radio's lock is set now (the latest SR reply).
bool tx_scheduler_swr_tripped(void);
// Tune ended on a trip: the TX HUD shows it for the rest of the slot.
void tx_scheduler_swr_abort_note(void);
// Why the TX HUD shows an abort now, or null.
const char *tx_scheduler_hud_abort_reason(void);

// Operate TX HUD: active while CAT tones are in flight; snapshot is the
// armed/in-flight message text + on-air offset (power/SWR filled by the caller).
bool tx_scheduler_hud_active(void);
bool tx_scheduler_hud_snapshot(const char **text_out, int *offset_hz_out);
