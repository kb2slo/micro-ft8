#include "dial_history.h"
#include "decode_pipeline.h"
#include "decode_limits.h"
#include "decode_core_busy.h"
#include "decode_fields.h"
#include "decode_sync.h"
#include "heard_grid.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "autoseq.h"
#include "callsign_hashtable.h"
#include "decode_sort.h"
#include "decode_tx_state.h"
#include "debug_log.h"
#include "protocol.h"
#include "rtc_core.h"

extern "C" {
#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/message.h"
#include "decode_retry.h"
#include "decode_snr.h"
#include "decode_subtract.h"
}

#include "adif.h"

static const char* TAG = "DECODE";

// Bare forward declarations for the seams this pipeline needs but doesn't
// own: the main board's own file (poe_p4/main) defines each.
//
//   arm_from_autoseq/ignorelist_matches_normalized_dxcall: the
//     TX-arming trigger and its ignorelist filter (the P4 has no ignorelist
//     yet, row j4xh).
//   porta_emit_decode: the sidekick link. Safe to call unconditionally --
//     no-op until porta_start() has run.
//
// Each is called from decode_pipeline_apply(), on the main loop.
void arm_from_autoseq();
bool ignorelist_matches_normalized_dxcall(const std::string& dxcall_norm);
void porta_emit_decode(uint32_t decode_id, const char* text, int snr, int offset_hz,
                       float dt_s, bool is_cq, bool is_to_me, bool is_recent_qso,
                       uint32_t slot_idx, uint8_t slot_len_ds, uint32_t dial_hz);

AdifLoggerDedupe s_adif_logger_dedupe;

// autoseq refuses a recently logged call a new QSO for this long, and expires
// its parked entry after the same span; unequal, one outlives the other.
static_assert(AUTOSEQ_INACTIVE_EXPIRY_MS == kAdifLoggerDedupeWindowMs,
              "parked-entry expiry must match the recently-worked window");

bool decode_pipeline_recently_logged(const std::string& dxcall_norm) {
  return adif_logger_dedupe_is_duplicate(&s_adif_logger_dedupe, dxcall_norm, rtc_now_ms());
}

// ---- Heard grids (heard_grid.h). Main loop only, so not locked:
// decode_pipeline_apply() notes each decode, and autoseq reads the table when
// a QSO logs. Times are uptime, not rtc_now_ms(): the table empties on reboot
// anyway, so it never waits on the clock being set. ----
bool decode_pipeline_heard_grid(const std::string& dxcall_norm, std::string* grid_out) {
  return heard_grid_lookup(dxcall_norm.c_str(), esp_timer_get_time() / 1000, grid_out);
}

// ---- Recent decodes, keyed by the id handed out with EVENT DECODE (docs/architecture/webapp.md (Screens)) so Port A's QUEUE_REPLY can name one without re-parsing rendered text
// on the phone. A ring, not a map: bounded and allocation-free like
// everything else on this path. An id that has scrolled out of the ring
// resolves to nothing, which porta_host_queue_reply reports plainly rather
// than guessing. Main loop only, so not locked: decode_pipeline_apply()
// writes it and QUEUE_REPLY reads it. ----
constexpr int kDecodeIdRingLen = 64;
struct DecodeIdEntry {
  uint32_t id = 0;  // 0 = empty slot
  UiRxLine rx;
};
static DecodeIdEntry s_decode_ring[kDecodeIdRingLen];
static int s_decode_ring_next = 0;
static uint32_t s_next_decode_id = 1;  // 0 is "not a real decode"

// Assigns the next id, remembers enough of `e` to reconstruct a UiRxLine for
// a later reply, and returns the id for porta_emit_decode() to carry.
static uint32_t remember_decode_for_reply(const RxDecodeEntry& e) {
  uint32_t id = s_next_decode_id++;
  if (s_next_decode_id == 0) {
    s_next_decode_id = 1;  // skip the sentinel on wrap
  }
  DecodeIdEntry& slot = s_decode_ring[s_decode_ring_next];
  s_decode_ring_next = (s_decode_ring_next + 1) % kDecodeIdRingLen;
  slot.id            = id;
  slot.rx.text        = e.text;
  slot.rx.field1      = e.field1;
  slot.rx.field2      = e.field2;
  slot.rx.field3      = e.field3;
  slot.rx.snr         = e.snr;
  slot.rx.offset_hz   = e.offset_hz;
  slot.rx.slot_id     = e.slot_id;
  slot.rx.is_cq       = e.is_cq;
  slot.rx.is_to_me    = e.is_to_me;
  slot.rx.is_recent_qso = e.is_recent_qso;
  return id;
}

bool find_decode_for_reply(uint32_t decode_id, UiRxLine* out) {
  if (decode_id == 0) return false;
  for (int i = 0; i < kDecodeIdRingLen; ++i) {
    if (s_decode_ring[i].id == decode_id) {
      *out = s_decode_ring[i].rx;
      return true;
    }
  }
  return false;
}

static void normalize_call_token(std::string& s) {
  // trim <> wrappers used for hashed nonstd calls
  if (!s.empty() && s.front() == '<') s.erase(s.begin());
  if (!s.empty() && s.back()  == '>') s.pop_back();
  for (auto& ch : s) ch = (char)toupper((unsigned char)ch);
}

static bool rewrite_dxpedition_for_mycall(const std::string& raw_text,
                                          const std::string& mycall_up,
                                          std::string& rewritten_text) {
  std::istringstream iss(raw_text);
  std::string call1, rr73_tok, call2, foxcall, rpt;
  if (!(iss >> call1 >> rr73_tok >> call2 >> foxcall >> rpt)) return false;

  std::string trailing;
  if (iss >> trailing) return false;
  if (rr73_tok != "RR73;") return false;

  std::string call1_up = call1, call2_up = call2;
  normalize_call_token(call1_up);
  normalize_call_token(call2_up);
  if (call1_up.empty() || call2_up.empty() || mycall_up.empty()) return false;

  if (call1_up == mycall_up) {
    rewritten_text = call1 + " " + foxcall + " RR73";
    return true;
  }
  if (call2_up == mycall_up) {
    rewritten_text = call2 + " " + foxcall + " " + rpt;
    return true;
  }
  return false;
}

// ---- Static decode workspace (zero heap allocation) ----
// Use the shared RxDecodeEntry type from decode_types.h so we can hand it
// directly to porta_emit_decode without any conversion.
#define DEC_MAX       RX_MAX_DECODES       // 32
#define DEC_TEXT_MAX  RX_TEXT_MAX          // 64
#define DEC_FIELD_MAX RX_FIELD_MAX         // 20
typedef RxDecodeEntry DecodeMsg;

static DecodeMsg s_dec[DEC_MAX];
static int       s_dec_count;
// Each s_dec[] entry's ft8_lib FTX_MESSAGE_TYPE_STANDARD, for heard_grid_note().
static bool      s_dec_standard[DEC_MAX];

// ---- The decode task's hand-off to the main loop (docs/architecture/main_board.md, P4).
// The main loop owns autoseq, the armed TX, the decode-id ring and the
// recently-worked and heard-grid tables, so the decode task touches none of
// them: it publishes each slot's decodes here, and decode_pipeline_apply()
// does the rest. One slot deep, copied by value with no heap, under a mutex
// as porta's frame queue is. A result still waiting when the next is
// published means the main loop went a whole slot without a pass; the older
// one is dropped, and the drop is logged. ----
struct DecodeSlotResult {
  int64_t slot_idx;
  int64_t published_us;  // esp_timer
  int count;
  DecodeMsg dec[DEC_MAX];
  bool standard[DEC_MAX];
};
// Both copies of a slot's result (this one and the main loop's in decode_pipeline_apply()) are
// allocated on first use from the heap, about 7 KB each (docs/architecture/memory.md lists
// them).
static DecodeSlotResult* s_result = nullptr;  // guarded by result_mutex()
static bool s_result_pending = false;         // guarded by result_mutex()

static DecodeSlotResult* alloc_slot_result() {
  return static_cast<DecodeSlotResult*>(
      heap_caps_malloc(sizeof(DecodeSlotResult), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// A wait this long is 25 main-loop passes: the loop was stalled, not busy.
constexpr int64_t kLateApplyMs = 250;

static SemaphoreHandle_t result_mutex() {
  static StaticSemaphore_t buffer;
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&buffer);
  return mutex;
}

// Decode task.
static void publish_slot_result(int64_t slot_idx) {
  int64_t dropped_slot = -1;
  xSemaphoreTake(result_mutex(), portMAX_DELAY);
  if (!s_result) s_result = alloc_slot_result();
  if (!s_result) {
    xSemaphoreGive(result_mutex());
    ESP_LOGE(TAG, "DECODE slot %lld lost: no RAM for the hand-off", (long long)slot_idx);
    debug_log_line("DECODE slot lost: no RAM for the hand-off");
    return;
  }
  if (s_result_pending) dropped_slot = s_result->slot_idx;
  s_result->slot_idx = slot_idx;
  s_result->published_us = esp_timer_get_time();
  s_result->count = s_dec_count;
  memcpy(s_result->dec, s_dec, sizeof(s_dec[0]) * (size_t)s_dec_count);
  memcpy(s_result->standard, s_dec_standard, sizeof(s_dec_standard[0]) * (size_t)s_dec_count);
  s_result_pending = true;
  xSemaphoreGive(result_mutex());
  if (dropped_slot >= 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "DECODE slot %lld dropped, never applied", (long long)dropped_slot);
    ESP_LOGW(TAG, "%s", buf);
    debug_log_line(buf);
  }
}

// Main loop.
static bool take_slot_result(DecodeSlotResult* out) {
  xSemaphoreTake(result_mutex(), portMAX_DELAY);
  const bool pending = s_result_pending && s_result;
  if (pending) {
    out->slot_idx = s_result->slot_idx;
    out->published_us = s_result->published_us;
    out->count = s_result->count;
    memcpy(out->dec, s_result->dec, sizeof(s_result->dec[0]) * (size_t)s_result->count);
    memcpy(out->standard, s_result->standard, sizeof(s_result->standard[0]) * (size_t)s_result->count);
    s_result_pending = false;
  }
  xSemaphoreGive(result_mutex());
  return pending;
}

bool decode_pipeline_result_pending(void) {
  xSemaphoreTake(result_mutex(), portMAX_DELAY);
  const bool pending = s_result_pending;
  xSemaphoreGive(result_mutex());
  return pending;
}

// ---- The station call, the decode task's one input from the main loop: a
// copy the main loop sets (decode_pipeline_set_my_call()) and each decode
// reads once, so the decode task never reads g_call while a Station change
// rewrites it. ----
static char s_my_call[DEC_FIELD_MAX] = "";  // guarded by my_call_mutex()

static SemaphoreHandle_t my_call_mutex() {
  static StaticSemaphore_t buffer;
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&buffer);
  return mutex;
}

void decode_pipeline_set_my_call(const std::string& call) {
  xSemaphoreTake(my_call_mutex(), portMAX_DELAY);
  snprintf(s_my_call, sizeof(s_my_call), "%s", call.c_str());
  xSemaphoreGive(my_call_mutex());
}

// Plain-C normalize: strip <>, uppercase, write into out[out_sz].
static void dec_normalize_call(const char* src, char* out, int out_sz) {
  const char* p = src;
  if (*p == '<') ++p;
  int len = (int)strlen(p);
  if (len > 0 && p[len - 1] == '>') --len;
  if (len >= out_sz) len = out_sz - 1;
  for (int i = 0; i < len; ++i) out[i] = (char)toupper((unsigned char)p[i]);
  out[len] = '\0';
}

static int dec_sort_cmp(const void* a, const void* b) {
  const DecodeMsg* da = (const DecodeMsg*)a;
  const DecodeMsg* db = (const DecodeMsg*)b;
  const DecodeSortEntry ea = {da->is_to_me, da->is_recent_qso, da->is_cq, da->snr};
  const DecodeSortEntry eb = {db->is_to_me, db->is_recent_qso, db->is_cq, db->snr};
  return decode_sort_cmp(&ea, &eb);
}

// Sync step applied during this decode, in ms. Folded into the slot line and
// cleared there. The console UART is not written: a line per message at 115200
// was inside this function's timer, on the capture task.
static int s_sync_delta_ms = 0;

// FreeRTOS run-time counter at the start of this decode, and each core's idle
// task counter then. The P4 is dual-core; core 1's idle task exists.
// portGET_RUN_TIME_COUNTER_VALUE is esp_timer microseconds, uint32.
static uint32_t s_rt_at_decode = 0;
static uint32_t s_idle_at_decode[2] = {0, 0};

static void sample_decode_cores(void) {
  s_rt_at_decode = (uint32_t)portGET_RUN_TIME_COUNTER_VALUE();
  s_idle_at_decode[0] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
  s_idle_at_decode[1] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
}

// One line per receive slot, sent as a Port A log (the sidekick prints it).
// dec is esp_timer, sampled before that send: a clock step in this function
// must not land in the duration. left is the soft clock's time until the
// boundary. y is the sync step this slot, 0 if none. b is each core's busy
// percent over this decode (core 0, then core 1). The text fits
// PORTA_EVENT_TEXT_MAX (64). A line that does not is resent without y, so a
// truncated event cannot clip b into a different number.
static void log_slot_summary(int num_candidates, int max_cand, int msg_count, int tome,
                             int64_t decode_t0_us) {
  const int64_t dec_ms = (esp_timer_get_time() - decode_t0_us) / 1000;
  const int64_t slot_ms = g_protocol->slot_time_ms;
  const int64_t slot_idx = decode_tx_state_get_decode_slot_idx();
  int64_t left_ms = 0;
  if (slot_idx >= 0 && slot_ms > 0) left_ms = (slot_idx + 1) * slot_ms - rtc_now_ms();
  const int sync_ms = s_sync_delta_ms;
  s_sync_delta_ms = 0;
  const uint32_t elapsed = (uint32_t)portGET_RUN_TIME_COUNTER_VALUE() - s_rt_at_decode;
  const int busy0 = decode_core_busy_pct(
      (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0) - s_idle_at_decode[0], elapsed);
  const int busy1 = decode_core_busy_pct(
      (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1) - s_idle_at_decode[1], elapsed);
  // 128 covers a full int64 print. The event field keeps 64 characters.
  char buf[128];
  int n = snprintf(buf, sizeof(buf),
                   "SLOT %lld c%d/%d m%d t%d dec%lld left%lld y%d b%d/%d",
                   (long long)slot_idx, num_candidates, max_cand, msg_count, tome,
                   (long long)dec_ms, (long long)left_ms, sync_ms, busy0, busy1);
  if (n > 64) {
    snprintf(buf, sizeof(buf),
             "SLOT %lld c%d/%d m%d t%d dec%lld left%lld b%d/%d",
             (long long)slot_idx, num_candidates, max_cand, msg_count, tome,
             (long long)dec_ms, (long long)left_ms, busy0, busy1);
  }
  debug_log_line(buf);
}

void decode_monitor_results(monitor_t* mon, const monitor_config_t* cfg, const float* block_gain) {
  const int64_t decode_t0_us = esp_timer_get_time();
  sample_decode_cores();
  const int max_cand = DECODE_MAX_CANDIDATES;

  // ---- heap instrumentation ----
  size_t heap_entry = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t heap_entry_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  UBaseType_t stack_hw_entry = uxTaskGetStackHighWaterMark(NULL);
  ESP_LOGD(TAG, "DECODE_HEAP ENTER: free=%u largest=%u alltime_min=%u stack_hw=%u",
           (unsigned)heap_entry, (unsigned)heap_entry_largest,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned)stack_hw_entry);

  s_dec_count = 0;

  static ftx_candidate_t candidates[max_cand];
  static float seen_hz[DECODE_MAX_CANDIDATES];
  static decode_retry_t retry;
  decode_retry_reset(&retry);
  const int n_tones = (mon->wf.protocol == FTX_PROTOCOL_FT4) ? 4 : 8;
  int num_candidates = 0;

  // ---- slot index + once-per-slot hashtable maintenance ----
  int64_t slot_idx = (decode_tx_state_get_decode_slot_idx() >= 0) ? decode_tx_state_get_decode_slot_idx() : rtc_now_ms() / g_protocol->slot_time_ms;
  int slot_id = (int)(slot_idx & 1);

  static int64_t s_last_aged_slot = -1;
  if (slot_idx != s_last_aged_slot) {
    s_last_aged_slot = slot_idx;
    hashtable_age_all();
  }

  // ---- per-frequency noise for the SNR, before any subtraction (decode.md, SNR) ----
  static decode_snr_slot_t s_snr_slot;
  if (!decode_snr_prepare(&s_snr_slot, mon, block_gain)) {
    ESP_LOGW(TAG, "SNR not prepared: waterfall %d bins x freq_osr %d; SNRs read the floor",
             mon->wf.num_bins, mon->wf.freq_osr);
  }

  // ---- mycall uppercase (stack, not heap) ----
  char mycall_up[DEC_FIELD_MAX];
  xSemaphoreTake(my_call_mutex(), portMAX_DELAY);
  memcpy(mycall_up, s_my_call, sizeof(mycall_up));
  xSemaphoreGive(my_call_mutex());
  for (char* p = mycall_up; *p; ++p) *p = (char)toupper((unsigned char)*p);

  // ---- decode candidates into static s_dec[] ----
  const int kMaxDecoded = 50;
  static ftx_message_t decoded[kMaxDecoded];
  static ftx_message_t* decoded_hashtable[kMaxDecoded];
  for (int i = 0; i < kMaxDecoded; ++i) decoded_hashtable[i] = nullptr;
  int num_decoded = 0;

  // Two searches. Each decode lowers its tones; the second search reads what that uncovered.
  // Half the estimated SNR, including the neighbouring time subdivision (decode.md).
  for (int pass = 0; pass < DECODE_SUBTRACT_PASSES && s_dec_count < DEC_MAX; ++pass) {
    int want = max_cand / (pass + 1);
    if (want < 1) want = 1;
    int found = ftx_find_candidates(&mon->wf, want, candidates, 5);
    if (pass == 0) {
      num_candidates = found;
      ESP_LOGD(TAG, "Candidates found: %d", num_candidates);
    }
    if (pass == 0 && found <= 0) {
      ESP_LOGD(TAG, "No candidates found; keeping RX list");
      // No candidates means we processed the slot's audio but found nothing —
      // still counts as "applied" for the TX-trigger guard. Still handed over,
      // empty, so the main loop arms after it and a quiet band cannot stall
      // the CQ. Published before the marker, as below.
      publish_slot_result(slot_idx);
      if (decode_tx_state_get_decode_slot_idx() > decode_tx_state_get_decode_applied_slot_idx()) {
        decode_tx_state_set_decode_applied_slot_idx(decode_tx_state_get_decode_slot_idx());
      }
      decode_tx_state_set_decode_in_progress(false);
      log_slot_summary(0, max_cand, 0, 0, decode_t0_us);
      return;
    }

    int seen = 0;
    for (int i = 0; i < found && s_dec_count < DEC_MAX; ++i) {
    float freq_hz = (mon->min_bin + candidates[i].freq_offset +
                    candidates[i].freq_sub / (float)cfg->freq_osr) / mon->symbol_period;
    float time_s = (candidates[i].time_offset +
                   candidates[i].time_sub / (float)cfg->time_osr) * mon->symbol_period;
    // The other freq_osr subdivision of a signal we just decoded is the same station.
    if (decode_subtract_already(seen_hz, seen, freq_hz)) continue;
    // Failed before, and no subtraction has touched its bins: it would fail again.
    if (decode_retry_unchanged(&retry, &candidates[i], n_tones)) continue;

    ftx_message_t message;
    ftx_decode_status_t status;
    memset(&message, 0, sizeof(message));
    memset(&status, 0, sizeof(status));

    if (!ftx_decode_candidate(&mon->wf, &candidates[i], 25, &message, &status)) {
      decode_retry_note_failed(&retry, &candidates[i]);
      continue;
    }

    // payload/hash dedupe (open addressing)
    int idx_hash = (int)(message.hash % kMaxDecoded);
    bool found_empty = false, found_dup = false;
    for (int probe = 0; probe < kMaxDecoded; ++probe) {
      ftx_message_t* p = decoded_hashtable[idx_hash];
      if (!p) { found_empty = true; break; }
      if (p->hash == message.hash &&
          0 == memcmp(p->payload, message.payload, sizeof(message.payload))) {
        found_dup = true; break;
      }
      idx_hash = (idx_hash + 1) % kMaxDecoded;
    }
    // A duplicate payload is the same station again. Still subtract it, but do not list it twice.
    if (!found_dup && !found_empty) continue;
    if (!found_dup) {
      memcpy(&decoded[idx_hash], &message, sizeof(message));
      decoded_hashtable[idx_hash] = &decoded[idx_hash];
      ++num_decoded;
    }

    // decode to human text
    char text[FTX_MAX_MESSAGE_LENGTH] = {0};
    ftx_message_offsets_t offsets;
    ftx_message_rc_t urc = ftx_message_decode(&message, &hash_if, text, &offsets);
    if (urc != FTX_MESSAGE_RC_OK || text[0] == '\0') continue;

    // Before this message is subtracted: the SNR reads its tones.
    const int snr_q = decode_snr_message(&s_snr_slot, mon, &candidates[i], message.payload);

    if (seen < DECODE_MAX_CANDIDATES) seen_hz[seen++] = freq_hz;
    decode_subtract_message(&mon->wf, &candidates[i], message.payload,
                            DECODE_SUBTRACT_NEIGHBORS, DECODE_SUBTRACT_FRACTION);
    decode_retry_note_subtracted(&retry, &candidates[i]);
    if (found_dup) continue;

    // DXpedition rewrite (uses heap briefly via std::string — bounded, rare path)
    char final_text[DEC_TEXT_MAX];
    {
      std::string raw(text);
      std::string rewritten(text);
      if (rewrite_dxpedition_for_mycall(raw, mycall_up, rewritten)) {
        ESP_LOGI(TAG, "DXpedition raw match: %s", text);
      }
      strncpy(final_text, rewritten.c_str(), DEC_TEXT_MAX - 1);
      final_text[DEC_TEXT_MAX - 1] = '\0';
    }

    // UI text dedup (linear scan — 32 entries max, no hash map needed)
    int dup_idx = -1;
    for (int j = 0; j < s_dec_count; ++j) {
      if (strcmp(s_dec[j].text, final_text) == 0) { dup_idx = j; break; }
    }
    if (dup_idx >= 0) {
      if (snr_q > s_dec[dup_idx].snr) {
        s_dec[dup_idx].snr = snr_q;
        s_dec[dup_idx].offset_hz = (int)lrintf(freq_hz);
        s_dec[dup_idx].slot_id = slot_id;
      }
      continue;
    }

    ESP_LOGD(TAG, "Decoded[%d] t=%.2fs f=%.1fHz snr=%d : %s",
             s_dec_count, time_s, freq_hz, snr_q, final_text);

    // Fill static entry
    DecodeMsg* d = &s_dec[s_dec_count];
    // final_text is always NUL-terminated above; a same-size copy keeps -O2's stringop-truncation check quiet (B89).
    static_assert(sizeof(d->text) == sizeof(final_text), "RX list text and decode text buffers must match");
    memcpy(d->text, final_text, sizeof(d->text));
    d->snr = snr_q;
    d->offset_hz = (int)lrintf(freq_hz);
    d->slot_id = slot_id;
    d->time_s = time_s;

    decode_fields_fill(d);
    s_dec_standard[s_dec_count] = ftx_message_get_type(&message) == FTX_MESSAGE_TYPE_STANDARD;

    d->is_cq = (strncmp(d->text, "CQ ", 3) == 0 || strcmp(d->text, "CQ") == 0);

    char f1_norm[DEC_FIELD_MAX];
    dec_normalize_call(d->field1, f1_norm, DEC_FIELD_MAX);
    d->is_to_me = (mycall_up[0] != '\0' && strcmp(f1_norm, mycall_up) == 0);

    // The recently-worked table is the main loop's: decode_pipeline_apply() sets this.
    d->is_recent_qso = false;

    s_dec_count++;
    }
    // Nothing subtracted leaves the waterfall as it was, so another search
    // would only retry candidates that just failed.
    if (seen == 0) break;
  }

  ESP_LOGD(TAG, "Decoded %d unique messages", s_dec_count);

  // ---- Auto sync soft RTC from decode timing ----
  {
    float times_s[DEC_MAX];
    for (int i = 0; i < s_dec_count; ++i) times_s[i] = s_dec[i].time_s;
    const int delta_ms =
        decode_sync_step_ms(times_s, s_dec_count, decode_tx_state_get_capture_clean() ? 1 : 0);
    if (delta_ms != 0) {
      rtc_apply_decode_sync(delta_ms);
      s_sync_delta_ms = delta_ms;
    }
  }

  // ---- Hand the slot to the main loop (decode_pipeline_apply()) ----
  publish_slot_result(slot_idx);

  // ---- heap instrumentation (exit) ----
  {
    size_t heap_exit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_exit_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    UBaseType_t stack_hw_exit = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGD(TAG, "DECODE_HEAP EXIT: free=%u largest=%u alltime_min=%u stack_hw=%u (delta_free=%d)",
             (unsigned)heap_exit, (unsigned)heap_exit_largest,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)stack_hw_exit,
             (int)heap_exit - (int)heap_entry);
  }

  int tome = 0;
  for (int i = 0; i < s_dec_count; ++i) if (s_dec[i].is_to_me) tome++;
  log_slot_summary(num_candidates, max_cand, s_dec_count, tome, decode_t0_us);

  // Publish, then mark this slot's decode applied, then clear the in-progress
  // flag, in that order. The TX trigger on core 0 reads in_progress and the
  // marker, then decode_pipeline_result_pending(): once it sees the marker, the
  // slot's result is already waiting for decode_pipeline_apply(), and the
  // trigger holds until the main loop has applied it.
  if (decode_tx_state_get_decode_slot_idx() > decode_tx_state_get_decode_applied_slot_idx()) {
    decode_tx_state_set_decode_applied_slot_idx(decode_tx_state_get_decode_slot_idx());
  }
  decode_tx_state_set_decode_in_progress(false);
}

void decode_pipeline_apply(void) {
  static DecodeSlotResult* rp = nullptr;  // the main loop's copy; too big for its stack
  if (!rp) {
    if (!decode_pipeline_result_pending()) return;  // nothing yet; allocate with the first result
    rp = alloc_slot_result();
    if (!rp) {
      ESP_LOGE(TAG, "DECODE apply: no RAM for the main loop's copy");
      return;
    }
  }
  DecodeSlotResult& r = *rp;
  if (!take_slot_result(&r)) return;

  const int64_t wait_ms = (esp_timer_get_time() - r.published_us) / 1000;
  if (wait_ms >= kLateApplyMs) {
    char buf[64];
    snprintf(buf, sizeof(buf), "DECODE slot %lld applied %lld ms late", (long long)r.slot_idx,
             (long long)wait_ms);
    ESP_LOGW(TAG, "%s", buf);
    debug_log_line(buf);
  }

  // ---- Heard grids and the recently-worked flag, both the main loop's tables ----
  const int64_t now_ms = rtc_now_ms();
  const int64_t uptime_ms = esp_timer_get_time() / 1000;
  for (int i = 0; i < r.count; ++i) {
    DecodeMsg* d = &r.dec[i];
    heard_grid_note(d->field2, d->field3, r.standard[i], uptime_ms);
    char dxnorm[DEC_FIELD_MAX];
    dec_normalize_call(d->field2, dxnorm, DEC_FIELD_MAX);
    d->is_recent_qso = (dxnorm[0] != '\0') &&
                       adif_logger_dedupe_is_duplicate(&s_adif_logger_dedupe, std::string(dxnorm), now_ms);
  }

  // ---- Sort in-place: to_me, fresh CQ, other fresh, recent QSOs ----
  // After the loop above: r.standard[] follows the unsorted order.
  qsort(r.dec, r.count, sizeof(DecodeMsg), dec_sort_cmp);

  // ---- Autoseq: build small to_me vector at boundary (only to_me entries) ----
  if (!decode_tx_state_get_was_txing()) {
    std::vector<UiRxLine> to_me_auto;
    for (int i = 0; i < r.count; ++i) {
      if (!r.dec[i].is_to_me) break;  // sorted, so once we pass to_me we're done
      char dxnorm[DEC_FIELD_MAX];
      dec_normalize_call(r.dec[i].field2, dxnorm, DEC_FIELD_MAX);
      if (ignorelist_matches_normalized_dxcall(std::string(dxnorm))) {
        ESP_LOGI(TAG, "IgnoreList: skip auto reply to %s", dxnorm);
        continue;
      }
      // A recently worked call is not dropped here: autoseq refuses it a new
      // QSO itself (decode_pipeline_recently_logged), and a QSO still in the
      // queue needs the station's closing 73 -- which arrives after our RR73
      // has already logged it.
      UiRxLine rx;
      rx.text = r.dec[i].text;
      rx.field1 = r.dec[i].field1;
      rx.field2 = r.dec[i].field2;
      rx.field3 = r.dec[i].field3;
      rx.snr = r.dec[i].snr;
      rx.offset_hz = r.dec[i].offset_hz;
      rx.slot_id = r.dec[i].slot_id;
      rx.is_cq = r.dec[i].is_cq;
      rx.is_to_me = true;
      to_me_auto.push_back(std::move(rx));
    }

    if (!to_me_auto.empty()) {
      autoseq_on_decodes(to_me_auto);
    }

    arm_from_autoseq();
  }

  // ---- Hand the sorted list to the sidekick link ----
  if (r.count > 0) {
    // Each decode carries the slot its audio was recorded in, and the dial frequency
    // at the middle of its own signal (Port A v2). A signal only decodes if
    // most of it was received on one frequency, so when the operator retunes
    // partway through the slot, this labels each decode with the frequency
    // it was actually heard on, and none has to be dropped. time_s counts
    // from the start of the recording, which begins on the slot boundary.
    const int64_t emit_slot_idx = r.slot_idx;
    const int64_t emit_slot_ms = g_protocol->slot_time_ms;
    const int64_t half_signal_ms =
        (int64_t)(g_protocol->total_symbols * g_protocol->symbol_period * 500.0f);
    for (int i = 0; i < r.count; ++i) {
      const RxDecodeEntry& e = r.dec[i];
      uint32_t decode_id = remember_decode_for_reply(e);
      const int64_t mid_ms =
          emit_slot_idx * emit_slot_ms + (int64_t)(e.time_s * 1000.0f) + half_signal_ms;
      porta_emit_decode(decode_id, e.text, e.snr, e.offset_hz, e.time_s,
                        e.is_cq, e.is_to_me, e.is_recent_qso,
                        (uint32_t)emit_slot_idx, (uint8_t)(emit_slot_ms / 100),
                        dial_history_at(mid_ms));
    }
  } else {
    ESP_LOGD(TAG, "No messages decoded; keeping RX list");
  }
}
