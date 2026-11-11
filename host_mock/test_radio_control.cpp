// radio_control's dispatcher (components/radio_control/radio_control.cpp): the NONE backend, which the P4 selects
// while the HL2 is the active radio (poe_p4/main/radio_select.cpp), keys and retunes nothing. A fake stands in for
// the QMX's ops table, so the test sees exactly which calls reach a radio.

#include <cstdio>
#include <cstring>

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
static int g_tx_dial = 0, g_rx_dial = 0;
static esp_err_t fake_begin_tx(int tx_dial, int, int rx_dial) { ++g_calls; g_tx_dial = tx_dial; g_rx_dial = rx_dial; return ESP_OK; }
static esp_err_t fake_tone(float) { ++g_calls; return ESP_OK; }
static esp_err_t fake_ok(void) { ++g_calls; return ESP_OK; }
static esp_err_t fake_tune(bool, int, int) { ++g_calls; return ESP_OK; }

// What the fake radio reports (IF;): its frequency, whether it transmits, and how many times that changed.
static int g_radio_hz = 0;
static bool g_radio_tx = false;
static uint32_t g_radio_changes = 0;
static bool fake_get_state(radio_state_t* out)
{
    if (g_radio_hz <= 0) return false;
    out->freq_hz = g_radio_hz;
    out->transmitting = g_radio_tx;
    out->mode = 6;
    out->split = false;
    return true;
}
static uint32_t fake_state_changes(void) { return g_radio_changes; }
static void radio_reports(int hz, bool tx = false)
{
    g_radio_hz = hz;
    g_radio_tx = tx;
    ++g_radio_changes;
}

static const radio_control_capabilities_t k_fake_caps = {true, true, true, true, true, true};
// Not const: dial() takes its state reports away to stand for a radio that cannot report.
static radio_control_ops_t g_fake_qmx = [] {
    radio_control_ops_t ops = {};
    ops.name = "qmx";
    ops.capabilities = &k_fake_caps;
    ops.ready = fake_ready;
    ops.sync_frequency_mode = fake_ok_freq;
    ops.begin_tx = fake_begin_tx;
    ops.set_tone_hz = fake_tone;
    ops.end_tx = fake_ok;
    ops.set_tune = fake_tune;
    ops.get_state = fake_get_state;
    ops.state_changes = fake_state_changes;
    return ops;
}();

const radio_control_ops_t* radio_control_qmx_get_ops(void) { return &g_fake_qmx; }

// The clock is not under test here, and the history only by what is noted and how often.
static int g_history_notes = 0;
static uint32_t g_history_hz = 0;
int64_t rtc_now_ms() { return 0; }
void dial_history_note(int64_t, uint32_t hz) { ++g_history_notes; g_history_hz = hz; }

// The dial's history holds what the radio reports, not what was asked for (radio.md, The dial a decode was heard on).
static void dial()
{
    radio_control_set_backend(RADIO_CONTROL_QMX);
    g_history_notes = 0;
    radio_reports(5357000);
    radio_control_dial_tick();
    expect(g_history_notes == 1 && g_history_hz == 5357000, "the radio's report is noted");
    radio_control_dial_tick();
    expect(g_history_notes == 1, "and once: nothing changed since");

    expect(radio_control_sync_frequency_mode(3573000) == ESP_OK, "a tune the radio has no band for still sends");
    expect(g_history_notes == 1, "a radio that reports is not noted where we asked");
    radio_reports(5357000);  // the IF; after the sync: the radio refused FA and stayed
    radio_control_dial_tick();
    expect(g_history_hz == 5357000, "a refused tune leaves the history on the radio's frequency");

    radio_reports(5354000);  // a knob turn, pushed with AI2;
    radio_control_dial_tick();
    expect(g_history_hz == 5354000, "a knob turn is noted");

    g_history_notes = 0;
    expect(radio_control_begin_tx(7047500, 1500, 7074000) == ESP_OK, "a transmission retunes");
    radio_reports(7047500, true);
    radio_control_dial_tick();
    expect(g_history_notes == 0, "the dial a transmission retuned to is not noted");
    radio_control_end_tx();
    radio_reports(7074000);
    radio_control_dial_tick();
    expect(g_history_notes == 1 && g_history_hz == 7074000, "the report back on the receive dial is");

    g_history_notes = 0;
    radio_reports(7074000, true);  // TX on the receive dial: not retuned, still not the operator's
    radio_control_dial_tick();
    expect(g_history_notes == 0, "a report while transmitting is not noted");
    radio_reports(7074000);
    radio_control_dial_tick();
    expect(g_history_notes == 1, "the one after it is");

    g_fake_qmx.get_state = nullptr;
    g_fake_qmx.state_changes = nullptr;
    expect(radio_control_sync_frequency_mode(14074000) == ESP_OK && g_history_hz == 14074000,
           "a radio that cannot report is noted where we set it");
    g_fake_qmx.get_state = fake_get_state;
    g_fake_qmx.state_changes = fake_state_changes;
}

// A transmission that retunes (tx_timing.h tx_plan(); radio.md, TX timing on a QMX): the backend gets both dials, the dispatcher
// says the radio is retuned until end_tx(), and the dial's history, which labels decodes, keeps the receive dial.
static void retune()
{
    radio_control_set_backend(RADIO_CONTROL_QMX);
    g_history_notes = 0;
    expect(radio_control_begin_tx(14074000, 7500, 14074000) == ESP_OK && radio_control_tx_retuned_hz() == 0,
           "a transmission on the receive dial is not a retune");
    radio_control_end_tx();
    expect(radio_control_begin_tx(7047500, 1500, 7074000) == ESP_OK, "40 m's FT4 keys");
    expect(g_tx_dial == 7047500 && g_rx_dial == 7074000, "the backend gets the transmit and receive dials");
    expect(radio_control_tx_retuned_hz() == 7047500, "the radio reads as retuned while it transmits");
    expect(radio_control_end_tx() == ESP_OK && radio_control_tx_retuned_hz() == 0, "and on its receive dial after");
    expect(g_history_notes == 0, "a retune is not noted in the dial's history");
}

int main()
{
    radio_control_set_backend(RADIO_CONTROL_QMX);
    expect(radio_control_ready() && radio_control_begin_tx(14074000, 1500, 14074000) == ESP_OK, "the QMX keys");
    radio_control_end_tx();
    retune();
    dial();

    radio_control_set_backend(RADIO_CONTROL_NONE);
    g_calls = 0;
    expect(!radio_control_ready(), "NONE is never ready");
    expect(radio_control_begin_tx(14074000, 1500, 14074000) != ESP_OK, "NONE refuses to key");
    expect(radio_control_begin_tx(7047500, 1500, 7074000) != ESP_OK && radio_control_tx_retuned_hz() == 0,
           "or to retune for a transmission");
    expect(radio_control_set_tone_hz(1500.0f) != ESP_OK, "or to sound a tone");
    expect(radio_control_set_tune(true, 14074000, 1500) != ESP_OK, "or to tune");
    expect(radio_control_sync_frequency_mode(14074000) != ESP_OK, "or to retune");
    expect(radio_control_end_tx() != ESP_OK, "and has nothing to un-key");
    radio_config_t cfg;
    expect(!radio_control_get_config(&cfg), "NONE reports no configuration");
    {
        radio_config_t c = {};
        snprintf(c.model, sizeof(c.model), "QMX 60-15m");
        c.band_count = 2;
        c.bands[0] = {60, 1, 0, 1, 0, 0};
        c.bands[1] = {40, 0, 0, 0, 0, -1};
        c.ptt_is_serial = 0;
        char text[256];
        radio_config_format(&c, text, sizeof(text));
        expect(strcmp(text, "radio_model=QMX 60-15m\nradio_bands=60 40\nradio_band_tx=1 0\n"
                            "radio_ptt=0100 000?\nradio_ptt_serial=0\n") == 0,
               "config reply lines: bands, Transmit and the four PTT outputs a band");
        char small[20];
        const size_t n = radio_config_format(&c, small, sizeof(small));
        expect(n == sizeof(small) - 1 && small[n] == 0, "a short buffer is filled and terminated");
    }
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
