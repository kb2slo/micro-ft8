// The chosen radio: what feeds channel 0 and what the station's controls act on (radio_select.h).
//
// Both transports stay installed once started: the USB host for the QMX, Ethernet for the HL2. Choosing a radio
// changes which one is active, so there is no teardown and no restart, and a QMX left plugged in is never reset by a
// change (a P4 reset can wedge its USB, docs/architecture/main_board.md, Host). When the QMX and the HL2 both become
// channels (RFC 0014 §7, item 12), this file is what is replaced: no component knows which radio is active.

#include <atomic>

#include "esp_log.h"

#include "decode_tx_state.h"
#include "radio_control.h"
#include "station.h"

#include "hl2_transport.h"
#include "qmx_transport.h"
#include "radio_select.h"
#include "tx_scheduler.h"

static const char* TAG = "radio_select";

static std::atomic<bool> s_qmx_active{false};
static bool s_qmx_started = false;

void radio_select_apply(int station_radio)
{
    // An armed transmission that has not keyed yet would key whichever backend is active when its slot comes.
    tx_scheduler_cancel();
    if (station_radio == kStationRadioHl2) {
        s_qmx_active = false;
        // The HL2 has no backend yet (receive only), so nothing can be keyed or retuned, a QMX still on the Host
        // port included.
        radio_control_set_backend(RADIO_CONTROL_NONE);
        if (!hl2_transport_set_active(true)) {
            ESP_LOGE(TAG, "the HL2 is chosen but Ethernet did not start: no radio is active");
        }
        ESP_LOGI(TAG, "radio: HL2");
        return;
    }
    hl2_transport_set_active(false);
    radio_control_set_backend(RADIO_CONTROL_QMX);
    if (!s_qmx_started) {
        qmx_transport_start();
        s_qmx_started = true;
    } else {
        // CAT may already be open: let the main loop push the station's band to the VFO, as on a CAT open.
        decode_tx_state_set_cdc_initial_sync_pending(true);
    }
    s_qmx_active = true;
    ESP_LOGI(TAG, "radio: QMX");
}

bool radio_select_qmx_feeds_decode(void)
{
    return s_qmx_active;
}
