#include "porta.h"
#include "porta_board.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "debug_log.h"

// Defined by each board's main (or porta_host stubs): the clock this stamps
// events with, and the control actions the sidekick can request. Setters
// return nullptr on success or a short reason for the operator's log / NAK.
int64_t rtc_now_ms();
uint32_t rtc_epoch_secs_or_zero();
const char* porta_host_set_clock(uint32_t epoch_secs, uint16_t millis);
void porta_host_tx_cancel(void);
// Snapshot Station.txt-shaped lines into out (NUL-terminated). Returns bytes
// written excluding NUL, or 0 on failure. Used for CONFIG_GET all / one.
size_t porta_host_config_snapshot(char* out, size_t out_cap);
// Apply one Station.txt key and persist. nullptr on success, else a short reason.
const char* porta_host_config_set(const char* key, const char* value);
// STATUS → 2 / → 4 equivalents for the companion.
const char* porta_host_connect(void);
const char* porta_host_tune(bool on);
// Call CQ, and change a queued entry's tries or the CQ's parity
// (docs/architecture/autoseq.md, Tries).
const char* porta_host_cq_call(void);
const char* porta_host_radio_read(void);
const char* porta_host_queue_tries(uint16_t entry_id, uint8_t tries);
const char* porta_host_queue_parity(uint16_t entry_id, uint8_t parity);
// active: cancel TX, clear the queue (the CQ with it), and key Tune when asked.
// !active: unkey Tune if this test turned it on.
void porta_host_link_test(bool active, bool tune);
// Cancel/reply by the stable ids QUEUE_ENTRY and EVENT DECODE hand out.
const char* porta_host_queue_cancel(uint16_t entry_id);
const char* porta_host_queue_reply(uint32_t decode_id);
// Both start an async FS operation and return immediately: nullptr means
// "accepted, rows and the terminal ACK/NAK arrive later via
// porta_emit_file_name()/porta_emit_file_entry()/porta_emit_ack()/
// porta_emit_nak()", not "done". A non-null return is a synchronous refusal
// (bad request, open failed) with no async reply to follow.
const char* porta_host_file_list_begin(uint8_t kind, uint16_t skip, uint8_t take);
const char* porta_host_file_read_begin(const char* filename, uint16_t skip, uint8_t take);
const char* porta_host_file_delete(const char* filename);
// Reset the main board's Station settings (and, with PORTA_FACTORY_RESET_LOGS, delete the QSO
// logs). nullptr on success, else why not; nothing is restarted.
const char* porta_host_factory_reset(uint8_t flags);
// SWR sweep (docs/architecture/radio.md, The SWR sweep): nullptr = accepted, progress follows as porta_emit_sweep() events; a
// non-null return is the refusal's reason and nothing was started.
const char* porta_host_sweep_start(const porta_sweep_range_t* ranges, size_t count);
#include "porta_proto.h"
#include "porta_out_queue.h"

namespace {

constexpr uart_port_t kPortaUart = UART_NUM_1;

// Shared with the sidekick (PORTA_BAUD in porta_proto.h). The UART opens at
// this rate; the link-test diagnostic is what may change it (row v3hh).
constexpr int kBaud = PORTA_BAUD;

// Big enough that one frame per tick never finds it full; see drain_out().
constexpr int kTxBufBytes = 4096;

// Both main loops sleep 10 ms and used to read 128 bytes per wake: about
// 12.8 kB/s, enough for 115200 and not for 230400 (measured on the P4,
// 2026-10-01). A reader task now decodes the byte stream itself. Link-test
// bulk frames are checked there. Every other frame is queued for porta_tick,
// which still owns autoseq and the radio. The task is not pinned. It sits
// above the USB host tasks (priority 5) so a short parse preempts them, then
// blocks in the read.
constexpr int kUartRxBytes = 4096;
constexpr UBaseType_t kRxTaskPrio = 6;
constexpr int kRxFrameQueueLen = 8;

const char* kTag = "PORTA";

bool s_running = false;
TaskHandle_t s_rx_task = nullptr;
std::atomic<bool> s_rx_stop{false};
std::atomic<bool> s_rx_hold{false};
std::atomic<bool> s_rx_paused{false};
std::atomic<bool> s_rx_exited{false};
std::atomic<uint32_t> s_rx_overrun{0};
// A mutex, not a spinlock. portENTER_CRITICAL masks interrupts while the
// reader waits, and at 1.5 Mbit/s the 128-byte FIFO is under 1 ms (failed
// 2026-10-01, ok 141, drop 9, crc 7). A mutex leaves the interrupt enabled.
SemaphoreHandle_t s_link_lock = nullptr;

void link_lock() {
  xSemaphoreTake(s_link_lock, portMAX_DELAY);
}

void link_unlock() {
  xSemaphoreGive(s_link_lock);
}
porta_frame_t s_rx_frames[kRxFrameQueueLen];
int s_rx_frame_head = 0;
int s_rx_frame_tail = 0;
int s_rx_frame_count = 0;
porta_decoder_t s_dec;
char s_version[PORTA_HELLO_VERSION_LEN + 1] = {};

inline uint32_t now_ms() {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool configure_uart() {
  gpio_num_t tx_pin = GPIO_NUM_NC;
  gpio_num_t rx_pin = GPIO_NUM_NC;
  porta_board_uart_pins(&tx_pin, &rx_pin);

  uart_config_t cfg = {};
  cfg.baud_rate = kBaud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
#ifdef UART_SCLK_REF_TICK
  cfg.source_clk = UART_SCLK_REF_TICK;
#else
  cfg.source_clk = UART_SCLK_DEFAULT;
#endif
  if (uart_param_config(kPortaUart, &cfg) != ESP_OK) return false;
  if (uart_set_pin(kPortaUart, tx_pin, rx_pin,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    return false;
  }
  uart_flush_input(kPortaUart);
  return true;
}

bool install_uart() {
  esp_err_t err = uart_driver_install(kPortaUart, kUartRxBytes, kTxBufBytes, 0, nullptr, 0);
  if (err == ESP_ERR_INVALID_STATE) {
    uart_driver_delete(kPortaUart);
    err = uart_driver_install(kPortaUart, kUartRxBytes, kTxBufBytes, 0, nullptr, 0);
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "UART driver install failed: %d", (int)err);
    return false;
  }
  if (!configure_uart()) {
    ESP_LOGW(kTag, "UART config failed");
    uart_driver_delete(kPortaUart);
    return false;
  }
  return true;
}

// Outbound queue (porta_out_queue.h): sized for a whole busy slot. Frames are
// queued from several tasks -- the decode task, the USB host task, and any
// task that logs through the debug_log relay -- and sent only by the main
// loop. Without the lock, the decode task on core 1 and drain_out() on core 0
// updated the head and count at once. A mutex, not a spinlock, for
// the reason s_link_lock gives, and because drain_out() holds it across
// uart_write_bytes(), which takes the driver's own mutex. Lock order: link,
// then out. Nothing run while holding it may take the link lock or queue a
// frame: the mutex is not recursive.
PortaOutQueue s_out;
SemaphoreHandle_t s_out_lock = nullptr;

void out_lock() {
  xSemaphoreTake(s_out_lock, portMAX_DELAY);
}

void out_unlock() {
  xSemaphoreGive(s_out_lock);
}

void enqueue(const uint8_t* bytes, size_t len) {
  out_lock();
  porta_out_queue_push(&s_out, bytes, len);
  out_unlock();
}

uint32_t out_dropped() {
  out_lock();
  const uint32_t dropped = porta_out_queue_dropped(&s_out);
  out_unlock();
  return dropped;
}

// Drain as many frames as the UART TX ring can take without blocking.
// CONFIG_GET-all enqueues dozens of VALUE frames in one handle_frame(); a
// hard "1 frame per tick" (the old rate limit) then took hundreds of ms to
// clear and still overflowed a short queue. The ring is 4 KiB; writing only
// what fits keeps the main loop non-blocking while clearing a burst in a few
// 10 ms ticks. Main loop only: every UART write stays on one task, which the
// link test's baud changes rely on.
void drain_out() {
  out_lock();
  for (const PortaOutFrame* f = porta_out_queue_front(&s_out); f != nullptr;
       f = porta_out_queue_front(&s_out)) {
    size_t free_bytes = 0;
    if (uart_get_tx_buffer_free_size(kPortaUart, &free_bytes) != ESP_OK) {
      break;
    }
    // Leave a little slack so a concurrent writer never sees a zero-free
    // ring; uart_write_bytes with ticks=0 would otherwise return short.
    if (free_bytes < f->len + 32u) {
      break;
    }
    const int wrote = uart_write_bytes(kPortaUart, (const char*)f->bytes, f->len);
    if (wrote < 0 || (size_t)wrote < f->len) {
      break;
    }
    porta_out_queue_pop(&s_out);
  }
  out_unlock();
}

// HELLO replaces the ad-hoc beacon. Same purpose -- report the companion's
// build, now framed and CRC-checked like everything else, and carrying a
// protocol version the old beacon had no room for.
void handle_hello(const porta_frame_t& f) {
  uint8_t proto = 0;
  char remote[PORTA_HELLO_VERSION_LEN + 1] = {};
  if (!porta_proto_parse_hello(&f, &proto, remote)) return;

  if (proto != PORTA_PROTO_VERSION) {
    // Said out loud rather than failing obscurely later: the two ends are
    // flashed independently, so a mismatch is a normal state mid-update.
    ESP_LOGW(kTag, "Companion speaks protocol v%u, we speak v%u", proto,
             (unsigned)PORTA_PROTO_VERSION);
  }

  if (strncmp(remote, s_version, PORTA_HELLO_VERSION_LEN) == 0) {
    return;   // unchanged; HELLO repeats every few seconds
  }
  strncpy(s_version, remote, sizeof(s_version) - 1);

  ESP_LOGI(kTag, "Companion version %s", remote);
}

// The control direction. Answered with ACK or NAK carrying the same verb, so
// the sidekick can match a reply to its request without a sequence number.
porta_link_run_t s_link;
bool s_link_radio = false;
uint32_t s_link_rx_bytes = 0;
uint32_t s_link_data_frames = 0;
uint32_t s_link_log_ms = 0;
bool s_link_logging = false;

void link_log(const char* why) {
  porta_link_status_t st;
  unsigned phase = 0;
  uint32_t data_frames = 0;
  uint32_t rxb = 0;
  const uint32_t crc = s_dec.crc_errors;
  link_lock();
  porta_link_run_status(&s_link, crc, &st);
  phase = (unsigned)s_link.phase;
  data_frames = s_link_data_frames;
  rxb = s_link_rx_bytes;
  link_unlock();
  uint32_t hw = 0;
  uart_get_baudrate(kPortaUart, &hw);
  ESP_LOGI(kTag,
           "link %s phase %u hw %" PRIu32 " tx %" PRIu32 " ok %" PRIu32
           " drop %" PRIu32 " dup %" PRIu32 " bad %" PRIu32 " crc %" PRIu32
           " data %" PRIu32 " rxb %" PRIu32,
           why, phase, hw, st.tx_bytes, st.frames_ok, st.drops,
           st.duplicates, st.mismatches, st.crc_errors, data_frames, rxb);
}

void link_log_tick() {
  link_lock();
  const bool active = porta_link_run_active(&s_link);
  link_unlock();
  const uint32_t now = now_ms();
  if (active) {
    if (!s_link_logging || (now - s_link_log_ms) >= 1000u) {
      link_log("run");
      s_link_log_ms = now;
      s_link_logging = true;
    }
    return;
  }
  if (s_link_logging) {
    link_log("done");
    s_link_logging = false;
  }
}

uint32_t s_link_hw_baud = PORTA_BAUD;
uint32_t s_link_pending_baud = 0;

void consume_rx(const uint8_t* buf, int len, bool queue_control);

// uart_flush_input while porta_rx is blocked in uart_read_bytes races the
// driver ring. Hold the task out of the read across a baud change. Bytes it
// already pulled are parsed first: they were clocked at the old rate.
// pdMS_TO_TICKS(5) is 0 at a 10 ms tick, and vTaskDelay(0) does not wait. The
// reader was still inside uart_read_bytes, the baud change ran anyway, and
// porta_rx then spun until the task watchdog fired (2026-10-01). The wait is
// 40 ms so a 1 ms tick still covers the 20 ms read timeout.
bool rx_hold_enter() {
  if (s_rx_task == nullptr) return true;
  s_rx_hold.store(true);
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(40);
  while (!s_rx_paused.load()) {
    if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
      s_rx_hold.store(false);
      ESP_LOGW(kTag, "porta_rx did not pause for baud change");
      return false;
    }
    vTaskDelay(1);
  }
  return true;
}

void rx_hold_leave() {
  s_rx_hold.store(false);
}

void rx_queue_frame(const porta_frame_t& frame) {
  link_lock();
  if (s_rx_frame_count >= kRxFrameQueueLen) {
    s_rx_overrun.fetch_add(1);
    link_unlock();
    return;
  }
  s_rx_frames[s_rx_frame_tail] = frame;
  s_rx_frame_tail = (s_rx_frame_tail + 1) % kRxFrameQueueLen;
  s_rx_frame_count++;
  link_unlock();
}

bool rx_take_frame(porta_frame_t* frame) {
  link_lock();
  if (s_rx_frame_count == 0) {
    link_unlock();
    return false;
  }
  *frame = s_rx_frames[s_rx_frame_head];
  s_rx_frame_head = (s_rx_frame_head + 1) % kRxFrameQueueLen;
  s_rx_frame_count--;
  link_unlock();
  return true;
}

void porta_rx_task(void* /*arg*/) {
  uint8_t buf[512];
  while (!s_rx_stop.load()) {
    if (s_rx_hold.load()) {
      s_rx_paused.store(true);
      while (s_rx_hold.load() && !s_rx_stop.load()) {
        vTaskDelay(1);
      }
      s_rx_paused.store(false);
      continue;
    }
    // Returns as soon as 512 bytes are waiting. The timeout only matters when
    // the wire is idle, so a stop or a baud change is noticed within 20 ms.
    const int n = uart_read_bytes(kPortaUart, buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (n <= 0) {
      // A driver that returns at once must not spin: this task is above idle.
      vTaskDelay(1);
      continue;
    }
    consume_rx(buf, n, true);
  }
  s_rx_exited.store(true);
  vTaskDelete(nullptr);
}

bool link_set_uart_baud(uint32_t baud) {
  if (!rx_hold_enter()) return false;
  uart_set_baudrate(kPortaUart, baud);
  uart_flush_input(kPortaUart);
  porta_decoder_resync(&s_dec);
  link_lock();
  s_link_hw_baud = baud;
  link_unlock();
  rx_hold_leave();
  return true;
}

void link_apply_baud(uint32_t baud) {
  if (!link_set_uart_baud(baud)) return;
  const uint32_t now = now_ms();
  link_lock();
  porta_link_run_applied(&s_link, now);
  link_unlock();
}

// See the sidekick copy: a short wait returns with the TX ring still full,
// and the baud change then clocks the set-rate frame out at the new rate.
void link_flush_pending_baud() {
  link_lock();
  const uint32_t baud = s_link_pending_baud;
  link_unlock();
  if (baud == 0) return;
  if (uart_wait_tx_done(kPortaUart, pdMS_TO_TICKS(5)) != ESP_OK) return;
  link_apply_baud(baud);
  link_lock();
  const bool applied = s_link_hw_baud == baud && s_link_pending_baud == baud;
  if (applied) {
    s_link_pending_baud = 0;
  }
  link_unlock();
  if (applied) {
    link_log("baud");
  }
}

void link_restore_resting() {
  link_lock();
  const bool busy = porta_link_run_active(&s_link) || s_link_pending_baud != 0 ||
                    s_link_hw_baud == PORTA_BAUD;
  link_unlock();
  if (busy) {
    return;
  }
  if (uart_wait_tx_done(kPortaUart, pdMS_TO_TICKS(5)) != ESP_OK) return;
  if (!link_set_uart_baud(PORTA_BAUD)) return;
  ESP_LOGI(kTag, "link baud %" PRIu32, (uint32_t)PORTA_BAUD);
}

void link_sync_radio() {
  link_lock();
  const bool active = porta_link_run_active(&s_link);
  const bool tune = s_link.tune;
  link_unlock();
  if (active == s_link_radio) {
    return;
  }
  if (active) {
    porta_host_link_test(true, tune);
    s_link_radio = true;
  } else {
    s_link_radio = false;
    porta_host_link_test(false, false);
  }
}

void link_note() {
  char note[PORTA_EVENT_TEXT_MAX + 1];
  link_lock();
  const size_t n = porta_link_run_take_note(&s_link, note, sizeof(note));
  link_unlock();
  if (n > 0) {
    debug_log_line_public(note);
  }
}

void link_on_frame(const porta_frame_t& f, bool host_side) {
  uint32_t seed = 0;
  uint32_t pass = 0;
  bool tune = false;
  const bool start = porta_proto_parse_link_start(&f, &seed, &tune, &pass);
  uint32_t set_baud = 0;
  const bool set_rate = porta_proto_parse_link_set_rate(&f, &set_baud);
  const bool stop = porta_proto_parse_link_stop(&f);
  unsigned phase_before = 0;
  unsigned phase_after = 0;
  uint32_t switch_baud = 0;
  const uint32_t now = now_ms();
  const uint32_t crc = s_dec.crc_errors;
  link_lock();
  phase_before = (unsigned)s_link.phase;
  porta_link_run_on_frame(&s_link, &f, now, crc, &switch_baud);
  if (f.type == PORTA_MSG_LINK_DATA) {
    s_link_data_frames++;
  }
  if (start) {
    s_link_rx_bytes = 0;
    s_link_data_frames = 0;
  }
  if (switch_baud != 0) {
    s_link_pending_baud = switch_baud;
  }
  phase_after = (unsigned)s_link.phase;
  link_unlock();
  if (start) {
    ESP_LOGI(kTag, "link start before %u after %u pass %" PRIu32, phase_before,
             phase_after, pass);
  } else if (f.type == PORTA_MSG_LINK_TEST) {
    if (set_rate) {
      ESP_LOGI(kTag, "link set-rate %" PRIu32 " phase %u", set_baud, phase_after);
    } else if (stop) {
      link_log("stop");
    } else if (f.len > 0) {
      ESP_LOGI(kTag, "link cmd %u len %u phase %u", f.payload[0], f.len, phase_after);
    }
  }
  if (host_side) {
    link_note();
    link_sync_radio();
  }
}

void link_pump() {
  uint8_t frame[PORTA_PROTO_MAX_FRAME];
  for (int n = 0; n < 16; ++n) {
    size_t free_bytes = 0;
    if (uart_get_tx_buffer_free_size(kPortaUart, &free_bytes) != ESP_OK ||
        free_bytes < PORTA_PROTO_MAX_FRAME) {
      break;
    }
    uint32_t apply = 0;
    const uint32_t now = now_ms();
    const uint32_t crc = s_dec.crc_errors;
    link_lock();
    const size_t wrote = porta_link_run_next(&s_link, now, crc, frame, sizeof(frame), &apply);
    if (apply != 0) {
      s_link_pending_baud = apply;
    }
    link_unlock();
    if (wrote == 0) {
      break;
    }
    uart_write_bytes(kPortaUart, (const char*)frame, wrote);
    if (apply != 0) {
      break;
    }
  }
  link_flush_pending_baud();
  link_restore_resting();
  link_log_tick();
  link_note();
  link_sync_radio();
}

void handle_action(const porta_frame_t& f) {
  if (f.len < 1) return;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const uint8_t verb = f.payload[0];

  link_lock();
  const bool testing = porta_link_run_active(&s_link);
  link_unlock();
  if (testing) {
    switch (verb) {
    case PORTA_ACT_TUNE:
    case PORTA_ACT_CQ_CALL:
    case PORTA_ACT_QUEUE_REPLY:
    case PORTA_ACT_SWEEP_START:
    case PORTA_ACT_FACTORY_RESET:
      enqueue(buf, porta_proto_encode_nak(verb, "link test", buf, sizeof(buf)));
      return;
    default:
      break;
    }
  }

  switch (verb) {
  case PORTA_ACT_SET_CLOCK: {
    uint32_t secs = 0;
    uint16_t ms = 0;
    if (!porta_proto_parse_set_clock(&f, &secs, &ms)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_set_clock(secs, ms);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_TX_CANCEL: {
    if (!porta_proto_parse_tx_cancel(&f)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    porta_host_tx_cancel();
    enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    return;
  }
  case PORTA_ACT_CONNECT: {
    if (!porta_proto_parse_connect(&f)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_connect();
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_TUNE: {
    bool on = false;
    if (!porta_proto_parse_tune(&f, &on)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_tune(on);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_RADIO_READ: {
    if (!porta_proto_parse_radio_read(&f)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_radio_read();
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_CQ_CALL: {
    if (!porta_proto_parse_cq_call(&f)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_cq_call();
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_QUEUE_TRIES:
  case PORTA_ACT_QUEUE_PARITY: {
    uint16_t entry_id = 0;
    uint8_t value = 0;
    const bool tries = verb == PORTA_ACT_QUEUE_TRIES;
    const bool ok = tries ? porta_proto_parse_queue_tries(&f, &entry_id, &value)
                          : porta_proto_parse_queue_parity(&f, &entry_id, &value);
    if (!ok) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = tries ? porta_host_queue_tries(entry_id, value)
                            : porta_host_queue_parity(entry_id, value);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_QUEUE_CANCEL: {
    uint16_t entry_id = 0;
    if (!porta_proto_parse_queue_cancel(&f, &entry_id)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_queue_cancel(entry_id);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_QUEUE_REPLY: {
    uint32_t decode_id = 0;
    if (!porta_proto_parse_queue_reply(&f, &decode_id)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_queue_reply(decode_id);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_FILE_DELETE: {
    char filename[PORTA_FILENAME_MAX + 1];
    if (!porta_proto_parse_file_delete(&f, filename)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_file_delete(filename);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_FACTORY_RESET: {
    uint8_t flags = 0;
    if (!porta_proto_parse_factory_reset(&f, &flags)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_factory_reset(flags);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  case PORTA_ACT_SWEEP_START: {
    porta_sweep_range_t ranges[PORTA_SWEEP_MAX_RANGES];
    size_t count = 0;
    if (!porta_proto_parse_sweep_start(&f, ranges, &count)) {
      enqueue(buf, porta_proto_encode_nak(verb, "malformed", buf, sizeof(buf)));
      return;
    }
    const char* why = porta_host_sweep_start(ranges, count);
    if (why) {
      enqueue(buf, porta_proto_encode_nak(verb, why, buf, sizeof(buf)));
    } else {
      enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
    }
    return;
  }
  default:
    enqueue(buf, porta_proto_encode_nak(verb, "unknown action", buf, sizeof(buf)));
    return;
  }
}

void handle_config_get(const porta_frame_t& f) {
  char key[PORTA_CONFIG_KEY_MAX + 1] = {};
  if (!porta_proto_parse_config_get(&f, key)) {
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_CONFIG_GET, "malformed", buf, sizeof(buf)));
    return;
  }

  // Static: the station snapshot can be ~1–2 KiB; keeping this on the
  // caller's stack blew the P4's default 3584-byte main task (B67 bench).
  static char snap[2048];
  size_t n = porta_host_config_snapshot(snap, sizeof(snap));
  if (n == 0) {
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_CONFIG_GET, "empty config", buf, sizeof(buf)));
    return;
  }
  // Link health, appended here rather than by each board's snapshot because
  // the counters are this component's own: frames from the sidekick that
  // failed CRC or decoded, and outbound frames dropped for a full queue.
  // GET-only like the board's live lines (station_key_known() rejects a SET).
  // The sidekick adds its matching sk_* counters when it relays the reply.
  // If they do not fit, the reply goes without them rather than not at all.
  const int m = snprintf(snap + n, sizeof(snap) - n,
                         "porta_crc_errors=%lu\nporta_frames_ok=%lu\nporta_dropped=%lu\n",
                         (unsigned long)s_dec.crc_errors, (unsigned long)s_dec.frames_ok,
                         (unsigned long)out_dropped());
  if (m > 0 && (size_t)m < sizeof(snap) - n) {
    n += (size_t)m;
  } else {
    snap[n] = '\0';
  }

  const bool want_all = (key[0] == '\0');
  bool found = false;
  size_t start = 0;
  while (start < n) {
    size_t end = start;
    while (end < n && snap[end] != '\n') {
      end++;
    }
    std::string line(snap + start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t eq = line.find('=');
    if (eq != std::string::npos && eq > 0 && eq < PORTA_CONFIG_KEY_MAX) {
      const std::string k = line.substr(0, eq);
      const std::string v = line.substr(eq + 1);
      if (want_all || k == key) {
        uint8_t buf[PORTA_PROTO_MAX_FRAME];
        const size_t wn =
            porta_proto_encode_config_value(k.c_str(), v.c_str(), buf, sizeof(buf));
        if (wn > 0) {
          enqueue(buf, wn);
          // Kick the UART TX ring now so a full Station.txt snapshot does
          // not need a 64-deep software queue.
          drain_out();
          found = true;
        }
        if (!want_all) {
          break;
        }
      }
    }
    start = (end < n) ? end + 1 : n;
  }

  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  if (!want_all && !found) {
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_CONFIG_GET, "unknown key", buf, sizeof(buf)));
    drain_out();
    return;
  }
  enqueue(buf, porta_proto_encode_ack(PORTA_MSG_CONFIG_GET, buf, sizeof(buf)));
  drain_out();
}

void handle_config_set(const porta_frame_t& f) {
  char key[PORTA_CONFIG_KEY_MAX + 1] = {};
  char value[PORTA_CONFIG_VALUE_MAX + 1] = {};
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  if (!porta_proto_parse_config_set(&f, key, value)) {
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_CONFIG_SET, "malformed", buf, sizeof(buf)));
    return;
  }
  const char* why = porta_host_config_set(key, value);
  if (why) {
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_CONFIG_SET, why, buf, sizeof(buf)));
  } else {
    enqueue(buf, porta_proto_encode_ack(PORTA_MSG_CONFIG_SET, buf, sizeof(buf)));
  }
}

// Unlike every handler above, success here does not enqueue a reply: the
// operation just started (a background listing, or a multi-tick file read)
// and main.cpp's async FS pump emits the rows and the terminal ACK/NAK once
// it actually finishes -- see porta_host_file_list_begin()'s comment.
void handle_file_list(const porta_frame_t& f) {
  uint8_t kind = 0;
  uint16_t skip = 0;
  uint8_t take = 0;
  if (!porta_proto_parse_file_list_req(&f, &kind, &skip, &take)) {
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_FILE_LIST, "malformed", buf, sizeof(buf)));
    return;
  }
  const char* why = porta_host_file_list_begin(kind, skip, take);
  if (why) {
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_FILE_LIST, why, buf, sizeof(buf)));
  }
}

void handle_file_read(const porta_frame_t& f) {
  char filename[PORTA_FILENAME_MAX + 1] = {};
  uint16_t skip = 0;
  uint8_t take = 0;
  if (!porta_proto_parse_file_read_req(&f, filename, &skip, &take)) {
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_FILE_READ, "malformed", buf, sizeof(buf)));
    return;
  }
  const char* why = porta_host_file_read_begin(filename, skip, take);
  if (why) {
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    enqueue(buf, porta_proto_encode_nak(PORTA_MSG_FILE_READ, why, buf, sizeof(buf)));
  }
}

void handle_frame(const porta_frame_t& f) {
  switch (f.type) {
  case PORTA_MSG_HELLO:       handle_hello(f);       break;
  case PORTA_MSG_ACTION:      handle_action(f);      break;
  case PORTA_MSG_CONFIG_GET:  handle_config_get(f);  break;
  case PORTA_MSG_CONFIG_SET:  handle_config_set(f);  break;
  case PORTA_MSG_FILE_LIST:   handle_file_list(f);   break;
  case PORTA_MSG_FILE_READ:   handle_file_read(f);   break;
  case PORTA_MSG_LINK_TEST:
  case PORTA_MSG_LINK_DATA:
    link_on_frame(f, true);
    break;
  default:
    ESP_LOGD(kTag, "Unhandled frame type 0x%02x len %u", f.type, f.len);
    break;
  }
}

void consume_rx(const uint8_t* buf, int len, bool queue_control) {
  if (len <= 0) return;
  link_lock();
  if (porta_link_run_active(&s_link)) {
    s_link_rx_bytes += (uint32_t)len;
  }
  link_unlock();
  porta_frame_t frame;
  for (int i = 0; i < len; ++i) {
    if (!porta_decoder_push(&s_dec, buf[i], &frame)) {
      continue;
    }
    const bool link = frame.type == PORTA_MSG_LINK_TEST || frame.type == PORTA_MSG_LINK_DATA;
    if (link) {
      // The reader checks bulk frames in order. Waiting for porta_tick dropped
      // the tail of a 2.5 Mbit/s run (2026-10-01, ok 284 of 400).
      link_on_frame(frame, !queue_control);
    } else if (queue_control) {
      rx_queue_frame(frame);
    } else {
      handle_frame(frame);
    }
  }
}

}  // namespace

// Stamped at emit, not at arrival. The queue can hold events through a burst,
// and a viewer stamping on arrival would pile a whole slot's decodes onto
// whatever second they happened to drain.
uint32_t host_epoch_secs() {
  return rtc_epoch_secs_or_zero();
}

void porta_emit_log(const char* text) {
  if (!s_running || !text) return;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_log(host_epoch_secs(), text, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_decode(uint32_t decode_id, const char* text, int snr, int offset_hz,
                       float dt_s, bool is_cq, bool is_to_me, bool is_recent_qso,
                       uint32_t slot_idx, uint8_t slot_len_ds, uint32_t dial_hz) {
  if (!s_running || !text) return;
  porta_decode_event_t ev = {};
  ev.epoch_secs = host_epoch_secs();
  ev.decode_id = decode_id;
  strncpy(ev.text, text, sizeof(ev.text) - 1);
  // Clamped rather than cast: an out-of-range value should read as an extreme,
  // not wrap round to a plausible-looking wrong one.
  ev.snr = (int8_t)(snr < -128 ? -128 : (snr > 127 ? 127 : snr));
  ev.offset_hz = (uint16_t)(offset_hz < 0 ? 0 : (offset_hz > 65535 ? 65535 : offset_hz));
  const long centis = (long)(dt_s * 100.0f);
  ev.dt_centis = (int16_t)(centis < -32768 ? -32768 : (centis > 32767 ? 32767 : centis));
  ev.is_cq = is_cq;
  ev.is_to_me = is_to_me;
  ev.is_recent_qso = is_recent_qso;
  // A slot counted on an unset clock names no real time: 0, like epoch_secs.
  ev.slot_idx = ev.epoch_secs ? slot_idx : 0;
  ev.slot_len_ds = slot_len_ds;
  ev.dial_hz = dial_hz;

  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_decode(&ev, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_queue_entry(uint16_t entry_id, uint8_t state, uint8_t tries_sent,
                            uint8_t try_limit, uint8_t flags, const char* dxcall) {
  if (!s_running) return;
  porta_queue_entry_event_t q = {};
  q.epoch_secs = host_epoch_secs();
  q.entry_id = entry_id;
  q.state = state;
  q.tries_sent = tries_sent;
  q.try_limit = try_limit;
  q.flags = flags;
  if (dxcall) {
    strncpy(q.dxcall, dxcall, sizeof(q.dxcall) - 1);
  }
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_queue_entry(&q, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_slot_state(uint8_t slot_parity, uint16_t resolved_offset_hz) {
  if (!s_running) return;
  porta_slot_state_event_t s = {};
  s.epoch_secs = host_epoch_secs();
  s.slot_parity = slot_parity;
  s.resolved_offset_hz = resolved_offset_hz;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_slot_state(&s, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_usb_status(bool present, uint8_t kind, uint16_t vid, uint16_t pid) {
  if (!s_running) return;
  porta_usb_status_event_t u = {};
  u.epoch_secs = host_epoch_secs();
  u.present = present;
  u.kind = kind;
  u.vid = vid;
  u.pid = pid;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_usb_status(&u, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_radio_state(uint32_t freq_hz, bool tx, uint8_t mode, bool split) {
  if (!s_running) return;
  porta_radio_state_event_t r = {};
  r.epoch_secs = host_epoch_secs();
  r.freq_hz = freq_hz;
  r.tx = tx ? 1 : 0;
  r.mode = mode;
  r.split = split ? 1 : 0;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_radio_state(&r, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_activity(uint32_t slot_idx, uint8_t block_ms, uint8_t first,
                         const uint8_t* levels, const uint8_t* masks, uint8_t n,
                         uint16_t lo_hz, uint16_t hi_hz) {
  if (!s_running || !levels || !masks || n == 0) return;
  porta_activity_event_t a = {};
  a.epoch_secs = host_epoch_secs();
  a.slot_idx = slot_idx;
  a.block_ms = block_ms;
  a.first = first;
  a.n = n > PORTA_ACTIVITY_LEVELS_MAX ? PORTA_ACTIVITY_LEVELS_MAX : n;
  a.lo_hz = lo_hz;
  a.hi_hz = hi_hz;
  memcpy(a.level, levels, a.n);
  memcpy(a.mask, masks, (size_t)a.n * PORTA_ACTIVITY_MASK_BYTES);
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t len = porta_proto_encode_activity(&a, buf, sizeof(buf));
  enqueue(buf, len);
}

void porta_emit_sweep(uint8_t kind, uint8_t range_index, uint32_t freq_hz, int swr_hundredths,
                      uint8_t reason) {
  if (!s_running) return;
  porta_sweep_event_t e = {};
  e.epoch_secs = host_epoch_secs();
  e.kind = kind;
  e.range_index = range_index;
  e.freq_hz = freq_hz;
  e.swr_c = (int16_t)(swr_hundredths < -1 ? -1 : (swr_hundredths > 32767 ? 32767 : swr_hundredths));
  e.reason = reason;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_sweep_event(&e, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_tx_hud(bool active, bool aborted, bool writes_blocked,
                       const char* text, float power_w, float swr, int battery_pct,
                       int offset_hz, const char* reason) {
  if (!s_running) return;
  porta_tx_hud_event_t h = {};
  h.epoch_secs = host_epoch_secs();
  h.active = active;
  h.aborted = aborted;
  h.writes_blocked = writes_blocked;
  h.power_dw = (power_w < 0.f) ? -1 : (int16_t)(power_w * 10.f + 0.5f);
  h.swr_c = (swr < 0.f) ? -1 : (int16_t)(swr * 100.f + 0.5f);
  h.battery_pct = (int8_t)(battery_pct < -1 ? -1 : (battery_pct > 127 ? 127 : battery_pct));
  h.offset_hz = (uint16_t)(offset_hz < 0 ? 0 : offset_hz);
  if (text) {
    strncpy(h.text, text, sizeof(h.text) - 1);
  }
  if (reason) {
    strncpy(h.reason, reason, sizeof(h.reason) - 1);
  }
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_tx_hud(&h, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_tx_not_keyed(const char* text, int offset_hz, const char* reason) {
  if (!s_running) return;
  porta_tx_hud_event_t h = {};
  h.epoch_secs = host_epoch_secs();
  h.active = true;
  h.not_keyed = true;
  h.power_dw = -1;
  h.swr_c = -1;
  h.battery_pct = -1;
  h.offset_hz = (uint16_t)(offset_hz < 0 ? 0 : offset_hz);
  if (text) {
    strncpy(h.text, text, sizeof(h.text) - 1);
  }
  if (reason) {
    strncpy(h.reason, reason, sizeof(h.reason) - 1);
  }
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_tx_hud(&h, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_file_name(const char* name) {
  if (!s_running || !name) return;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_file_name_row(name, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_file_entry(const porta_qso_entry_row_t& row) {
  if (!s_running) return;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  const size_t n = porta_proto_encode_file_entry_row(&row, buf, sizeof(buf));
  enqueue(buf, n);
}

void porta_emit_ack(uint8_t verb) {
  if (!s_running) return;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  enqueue(buf, porta_proto_encode_ack(verb, buf, sizeof(buf)));
}

void porta_emit_nak(uint8_t verb, const char* reason) {
  if (!s_running) return;
  uint8_t buf[PORTA_PROTO_MAX_FRAME];
  enqueue(buf, porta_proto_encode_nak(verb, reason, buf, sizeof(buf)));
}

uint32_t porta_crc_errors() { return s_dec.crc_errors; }
uint32_t porta_frames_ok() { return s_dec.frames_ok; }

void porta_start() {
  if (s_running) return;

  if (s_link_lock == nullptr) {
    s_link_lock = xSemaphoreCreateMutex();
    if (s_link_lock == nullptr) {
      ESP_LOGW(kTag, "link lock failed");
      return;
    }
  }
  if (s_out_lock == nullptr) {
    s_out_lock = xSemaphoreCreateMutex();
    if (s_out_lock == nullptr) {
      ESP_LOGW(kTag, "out lock failed");
      return;
    }
    porta_out_queue_init(&s_out);
  }

  gpio_num_t tx_pin = GPIO_NUM_NC;
  gpio_num_t rx_pin = GPIO_NUM_NC;
  porta_board_uart_pins(&tx_pin, &rx_pin);

  gpio_reset_pin(tx_pin);
  gpio_reset_pin(rx_pin);

  // A TX ring buffer is not optional now that this port transmits. With a
  // tx_buffer_size of 0, uart_write_bytes() blocks until every byte has left
  // the FIFO -- about 22 ms for a full frame at 115200, inside the slot loop.
  if (!install_uart()) return;

  porta_decoder_init(&s_dec);
  porta_link_run_init(&s_link);
  s_link_radio = false;
  s_version[0] = '\0';
  s_running = true;
  s_rx_stop.store(false);
  s_rx_hold.store(false);
  s_rx_paused.store(false);
  s_rx_exited.store(false);
  s_rx_overrun.store(0);
  s_rx_task = nullptr;
  s_rx_frame_head = 0;
  s_rx_frame_tail = 0;
  s_rx_frame_count = 0;
  if (xTaskCreate(porta_rx_task, "porta_rx", 4096, nullptr, kRxTaskPrio, &s_rx_task) == pdPASS) {
    ESP_LOGI(kTag, "porta_rx task");
  } else {
    s_rx_task = nullptr;
    ESP_LOGW(kTag, "porta_rx task failed; polling the UART");
  }
  debug_log_set_relay(porta_emit_log);
  ESP_LOGI(kTag, "Started on UART%d TX=G%d RX=G%d baud=%d",
           (int)kPortaUart, (int)tx_pin, (int)rx_pin, kBaud);
}

void porta_stop() {
  if (!s_running) return;
  s_rx_stop.store(true);
  s_rx_hold.store(false);
  if (s_rx_task != nullptr) {
    for (int i = 0; i < 10 && !s_rx_exited.load(); ++i) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_rx_task = nullptr;
  }
  uart_driver_delete(kPortaUart);
  porta_decoder_init(&s_dec);
  s_running = false;
  debug_log_set_relay(nullptr);
  ESP_LOGI(kTag, "Stopped");
}

bool porta_link_test_active() {
  return s_link_radio;
}

void porta_tick() {
  if (!s_running) return;

  if (s_rx_task != nullptr) {
    porta_frame_t frame;
    while (rx_take_frame(&frame)) {
      handle_frame(frame);
    }
  } else {
    uint8_t buf[128];
    const int len = uart_read_bytes(kPortaUart, buf, sizeof(buf), 0);
    consume_rx(buf, len, false);
  }

  drain_out();
  link_pump();

  // Report drops when the count moves. Without this the queue silently eats
  // events and the browser's view just looks thinner than the screen's, with
  // nothing to say why -- and the drop count is the only evidence that the
  // per-tick send rate is too slow for the traffic.
  static uint32_t s_reported_drops = 0;
  const uint32_t dropped = out_dropped();
  if (dropped != s_reported_drops) {
    s_reported_drops = dropped;
    ESP_LOGW(kTag, "Dropped %" PRIu32 " outbound events (queue full)", dropped);
  }

  // Link health. A wire quietly failing a share of its frames looks identical
  // to a quiet one from up here, so say so when the count moves.
  static uint32_t s_reported_crc = 0;
  if (s_dec.crc_errors != s_reported_crc) {
    s_reported_crc = s_dec.crc_errors;
    ESP_LOGW(kTag, "Link: %" PRIu32 " CRC errors, %" PRIu32 " frames ok",
             s_dec.crc_errors, s_dec.frames_ok);
  }

  static uint32_t s_reported_overrun = 0;
  const uint32_t overrun = s_rx_overrun.load();
  if (overrun != s_reported_overrun) {
    s_reported_overrun = overrun;
    ESP_LOGW(kTag, "porta control queue dropped %" PRIu32 " frames", overrun);
  }
}
