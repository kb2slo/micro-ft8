// radio_control's dispatcher (components/radio_control/radio_control.cpp): the NONE backend, which the P4 selects
// while the HL2 is the active radio (poe_p4/main/radio_select.cpp), keys and retunes nothing. A fake stands in for
// the QMX's ops table, so the test sees exactly which calls reach a radio.

#include <cstdio>

#include "dial_history.h"
#include "radio_control.h"
#include "radio_control_backend.h"
#include "rtc_core.h"

static int g_fails = 0;
static int g_calls = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static bool fake_ready(void) { ++g_calls; return true; }
static esp_err_t fake_ok_freq(int) { ++g_calls; return ESP_OK; }
static esp_err_t fake_begin_tx(int, int) { ++g_calls; return ESP_OK; }
static esp_err_t fake_tone(float) { ++g_calls; return ESP_OK; }
static esp_err_t fake_ok(void) { ++g_calls; return ESP_OK; }
static esp_err_t fake_tune(bool, int, int) { ++g_calls; return ESP_OK; }

static const radio_control_capabilities_t k_fake_caps = {true, true, true, true, true, true};
static const radio_control_ops_t k_fake_qmx = [] {
    radio_control_ops_t ops = {};
    ops.name = "qmx";
    ops.capabilities = &k_fake_caps;
    ops.ready = fake_ready;
    ops.sync_frequency_mode = fake_ok_freq;
    ops.begin_tx = fake_begin_tx;
    ops.set_tone_hz = fake_tone;
    ops.end_tx = fake_ok;
    ops.set_tune = fake_tune;
    return ops;
}();

const radio_control_ops_t* radio_control_qmx_get_ops(void) { return &k_fake_qmx; }

// A retune notes the dial; the clock and the history are not under test here.
int64_t rtc_now_ms() { return 0; }
void dial_history_note(int64_t, uint32_t) {}

int main()
{
    radio_control_set_backend(RADIO_CONTROL_QMX);
    expect(radio_control_ready() && radio_control_begin_tx(14074000, 1500) == ESP_OK, "the QMX keys");
    radio_control_end_tx();

    radio_control_set_backend(RADIO_CONTROL_NONE);
    g_calls = 0;
    expect(!radio_control_ready(), "NONE is never ready");
    expect(radio_control_begin_tx(14074000, 1500) != ESP_OK, "NONE refuses to key");
    expect(radio_control_set_tone_hz(1500.0f) != ESP_OK, "or to sound a tone");
    expect(radio_control_set_tune(true, 14074000, 1500) != ESP_OK, "or to tune");
    expect(radio_control_sync_frequency_mode(14074000) != ESP_OK, "or to retune");
    expect(radio_control_end_tx() != ESP_OK, "and has nothing to un-key");
    expect(g_calls == 0, "no call reaches the QMX's ops while NONE is selected");
    const radio_control_capabilities_t* caps = radio_control_get_capabilities();
    expect(caps && !caps->has_swr && !caps->has_wattmeter, "NONE has no capabilities");
    expect(radio_control_get_backend() == RADIO_CONTROL_NONE, "NONE is reported as the backend");

    radio_control_set_backend(RADIO_CONTROL_QMX);
    expect(radio_control_ready() && g_calls > 0, "switching back reaches the QMX again");

    if (g_fails) {
        fprintf(stderr, "test_radio_control: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("test_radio_control: ok\n");
    return 0;
}
