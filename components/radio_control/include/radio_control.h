#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_CONTROL_QMX = 0,
    // No radio under CAT control: every operation is unsupported and ready() is false. The HL2, until it has
    // a backend of its own (row mg9c, docs/roadmap/hl2-support.md), and any radio that must not be keyed.
    RADIO_CONTROL_NONE = 1,
} radio_control_backend_t;

typedef struct {
    bool has_wattmeter;
    bool has_swr;
    bool can_set_time;
    bool audio_is_uac;
    bool can_md8_tune;
    bool can_ps0;
    bool can_sr;  // SR; reads the SWR-protection lock (QMX 1_04_004 and later; the firmware may still refuse it)
} radio_control_capabilities_t;

// What an SWR sweep needs from the radio, one CAT command per call, none of them
// blocking on a reply (swr_sweep.h owns the timing). Replies come back through
// freq_reply()/swr_reply(), which hold a value only after the matching query and
// only until clear_replies(). QMX only; see docs/architecture/radio.md (QMX SWR Tune over CAT).
typedef struct {
    esp_err_t (*unkey)(void);            // RX;
    esp_err_t (*mode_digital)(void);     // MD6;
    esp_err_t (*set_frequency)(int hz);  // FA<hz>;
    esp_err_t (*query_frequency)(void);  // FA;
    esp_err_t (*key)(void);              // MD8; enters SWR Tune, which keys by itself
    esp_err_t (*query_swr)(void);        // SW;
    bool (*frequency_reply)(int* hz);
    bool (*swr_reply)(int* hundredths);
    void (*clear_replies)(void);
    esp_err_t (*restore)(int hz);        // MD6, FR0, FT0, FA: the operating dial again
} radio_swr_io_t;

// The radio's SWR-protection lock, one CAT command per call and none blocking on a reply
// (the caller owns the timing). The reply holds a value only after the matching query and
// only until the next reset of the radio's replies (a TX begin or end, a disconnect).
// QMX only; see docs/architecture/radio.md (SWR protection trips on a QMX).
typedef struct {
    esp_err_t (*query)(void);             // SR;
    bool (*reply)(bool* locked);          // the last SR reply
    esp_err_t (*clear)(void);             // SR0;, then SR; so the reply reads it back
    void (*poll_with_meters)(bool on);    // PC;SW;SR; instead of PC;SW; in the TX poll
} radio_swr_lock_t;

// What the radio says of itself (IF;): where it is, not where we put it. The reply holds a value
// only after the matching query and only until the next reset of the radio's replies.
typedef struct {
    int freq_hz;
    bool transmitting;
    int mode;   // the MD digit
    bool split;
} radio_state_t;

// The radio's own configuration, as it reports it (QMX: the Menu Manager read at link-up,
// architecture/radio.md, The QMX's settings, read at link-up). Flags: 1 on, 0 off, -1 not read.
#define RADIO_CONFIG_BANDS_MAX 16
typedef struct {
    int band_m;  // 160, 80, ... 6
    int8_t transmit;
    int8_t tx_ptt_5v;
    int8_t tx_ptt_ground;
    int8_t rx_ptt_5v;
    int8_t rx_ptt_ground;
} radio_band_config_t;

typedef struct {
    char model[24];  // the radio's band set, such as "QMX+ 160-6m"; empty when not read
    int band_count;
    radio_band_config_t bands[RADIO_CONFIG_BANDS_MAX];
    int8_t ptt_is_serial;  // 1 the PTT jack is a serial port instead (QMX Serial 6 on PTT)
} radio_config_t;

// What a radio_control_rx_path_tick() did.
typedef enum {
    RADIO_RX_PATH_NONE = 0,
    RADIO_RX_PATH_MATCHED,   // the radio reads back IQ mode
    RADIO_RX_PATH_MISMATCH,  // the radio reads back audio mode: nothing will decode
    RADIO_RX_PATH_NO_REPLY,  // the radio did not answer: a firmware without IQ mode
} radio_rx_path_event_t;

void radio_control_set_backend(radio_control_backend_t backend);
radio_control_backend_t radio_control_get_backend(void);
const char* radio_control_backend_name(radio_control_backend_t backend);
const radio_control_capabilities_t* radio_control_get_capabilities(void);

bool radio_control_ready(void);

// Asks the radio for its state (QMX IF;); ESP_ERR_NOT_SUPPORTED where it cannot say. Not a poll:
// the caller asks when the operator looks and after a sync, never during a slot.
esp_err_t radio_control_query_state(void);
// Asks the radio which firmware it runs (QMX VN;); ESP_ERR_NOT_SUPPORTED where it cannot say.
esp_err_t radio_control_query_version(void);
// The radio's firmware version, such as "1_04_004", once it has answered; false before that.
bool radio_control_get_version(char* out, size_t cap);
// The last IF reply; false when none has arrived since the radio's replies were last reset.
bool radio_control_get_state(radio_state_t* out);
// The radio's configuration once it has been read since this radio connected; false before that,
// after a read that went unanswered, and for a radio that cannot report it.
bool radio_control_get_config(radio_config_t* out);
// Sets the radio's PTT outputs for an amplifier keyed from its PTT jack (QMX: the Menu Manager write,
// architecture/radio.md, Setting the PTT outputs): on every band whose Transmit is on, `keying`'s TX
// output alone, and the jack not a serial port; then the radio is read again. Starts the write and
// returns nullptr, or says why not. Its progress is radio_control_config_write_status().
typedef enum { RADIO_PTT_GROUND = 1, RADIO_PTT_5V = 2 } radio_ptt_keying_t;
const char* radio_control_set_ptt_outputs(radio_ptt_keying_t keying);
// The last write's state for the config reply ("writing", "done 12", "nothing to write",
// "failed at <path>: ..."); false when none has run since this radio connected.
bool radio_control_config_write_status(char* out, size_t cap);
// `config` as the config reply's lines (architecture/webapp.md, Settings > Amplifier): radio_model,
// radio_bands (metres, space-separated, as active_bands), radio_band_tx (1, 0 or ? a band),
// radio_ptt (four characters a band: TX PTT +5V, TX PTT grounded, RX PTT +5V, RX PTT grounded, each
// 1, 0 or ?) and radio_ptt_serial. Writes at most `cap` bytes, always terminated; returns the length.
size_t radio_config_format(const radio_config_t* config, char* out, size_t cap);
// Asks the radio to push its state when it changes (QMX AI2;), or to stop (AI0;);
// ESP_ERR_NOT_SUPPORTED where it cannot. A radio without the command ignores it.
esp_err_t radio_control_set_push(bool on);
// How many times the radio's reported state has changed since boot, 0 where it cannot push.
// Compare with the count last seen, then read radio_control_get_state().
uint32_t radio_control_state_changes(void);

// Once per CAT link-up, sets the radio's receive output to I/Q (QMX `Q9 1;`) and reads it back; call
// it every main-loop pass. `busy`: a transmission, Tune, sweep or prime is running, so nothing is sent.
// RADIO_RX_PATH_NONE where the radio has no such choice. docs/architecture/radio.md, The QMX's
// receive output.
radio_rx_path_event_t radio_control_rx_path_tick(bool busy);

esp_err_t radio_control_on_audio_start(void);
// Tunes the radio. For a radio that cannot report its frequency, the dial's history (dial_history.h) notes freq_hz.
esp_err_t radio_control_sync_frequency_mode(int freq_hz);
// Call every main-loop pass: notes in the dial's history (dial_history.h), which labels decodes and the band a
// QSO is logged on, the frequency the radio last reported (QMX IF;, pushed with AI2; and asked after a sync). The
// radio's word, not what we asked for: it may have refused a tune, or the operator turned the knob. Not while it
// transmits or is retuned for a transmission.
void radio_control_dial_tick(void);
// tx_dial_hz other than rx_dial_hz retunes the radio for this transmission and back in radio_control_end_tx()
// (tx_timing.h, tx_plan()). Not a sync: the dial's history (dial_history.h) keeps rx_dial_hz.
esp_err_t radio_control_begin_tx(int tx_dial_hz, int tx_base_hz, int rx_dial_hz);
esp_err_t radio_control_set_tone_hz(float tone_hz);
esp_err_t radio_control_end_tx(void);
// The dial a transmission retuned the radio to, from radio_control_begin_tx() until radio_control_end_tx(); 0 when
// it is on its receive dial. What the radio reports meanwhile is ours, not the operator's.
int radio_control_tx_retuned_hz(void);
// RF off with PTT still held (QMX TA0;); ESP_ERR_NOT_SUPPORTED where end_tx() is the only stop.
esp_err_t radio_control_key_up(void);
// How long after the slot boundary this radio's first tone is due (tx_timing.h).
int radio_control_tone_lead_ms(void);
esp_err_t radio_control_set_tune(bool enable, int freq_hz, int tone_hz);
esp_err_t radio_control_set_time(int hour, int minute, int second);
// Transmit held off, for the length of a firmware update and before any restart the board starts itself
// (main_board.md, Firmware updates). While held nothing keys the radio: radio_control_begin_tx(),
// radio_control_begin_cpfsk_tx(), radio_control_set_tune(true) and the key of radio_control_swr_io() (the SWR
// sweep's and the amplifier prime's) are refused; what un-keys still reaches the radio. A cancel alone holds
// transmission off only to the end of its slot, and autoseq re-arms at once.
void radio_control_hold_tx(bool on);
bool radio_control_tx_held(void);
esp_err_t radio_control_begin_cpfsk_tx(float base_hz,
                                       const uint8_t* symbols,
                                       size_t symbol_count,
                                       float tone_spacing_hz,
                                       uint32_t samples_per_symbol);

// The sweep's radio operations, or NULL when the selected radio cannot sweep
// (no can_md8_tune). The pointer stays valid for the life of the program. Its key
// is refused while radio_control_hold_tx() is on, for a run already holding the pointer too.
const radio_swr_io_t* radio_control_swr_io(void);

// The SWR-protection lock's operations, or NULL when the selected radio has none (no can_sr).
const radio_swr_lock_t* radio_control_swr_lock(void);

void radio_control_reset_tx_power_swr(void);
void radio_control_poll_tx_power_swr(void);
bool radio_control_get_tx_power_swr(float* power_w, float* swr);

#ifdef __cplusplus
}
#endif
