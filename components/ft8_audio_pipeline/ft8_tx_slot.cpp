#include "ft8_tx_slot.h"

#include "decode_tx_state.h"

bool ft8_tx_slot_is_tx(bool ft4, bool station, int64_t slot_idx)
{
    if (ft4) return slot_idx == decode_tx_state_get_tx_ft4_period_idx();
    return station && slot_idx == decode_tx_state_get_tx_slot_idx();
}

void ft8_tx_slot_clear(bool ft4, bool station, int64_t slot_idx)
{
    if (!ft8_tx_slot_is_tx(ft4, station, slot_idx)) return;
    if (ft4) decode_tx_state_set_tx_ft4_period_idx(-1);
    else decode_tx_state_set_tx_slot_idx(-1);
}

void ft8_tx_slot_mark_applied(bool ft4, bool station, int64_t slot_idx)
{
    if (ft4 && slot_idx > decode_tx_state_get_ft4_applied_period_idx()) {
        decode_tx_state_set_ft4_applied_period_idx(slot_idx);
    }
    if (station && slot_idx > decode_tx_state_get_decode_applied_slot_idx()) {
        decode_tx_state_set_decode_applied_slot_idx(slot_idx);
    }
}
