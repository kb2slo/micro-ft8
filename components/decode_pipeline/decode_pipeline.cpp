#include "decode_pipeline.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "autoseq.h"
#include "board_power.h"
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
}

#include "adif.h"

static const char* TAG = "DECODE";

// Bare forward declarations for the per-board seams this pipeline needs but
// doesn't own -- same reasoning as ft8_audio_pipeline.cpp's ui_push_waterfall_row:
// each board supplies its own definition rather than this shared file
// REQUIRES-ing a board-only component.
//
//   g_call/g_tx_view_dirty/g_last_reply_text/g_rx_dirty: owned by whichever
//     board's main.cpp/app orchestrates the UI dispatch loop (ADV) or, on a
//     headless board, a small always-present definition nothing else reads
//     yet.
//   log_rxtx_line: ADV's storage-backed RX/TX log; a board with no storage
//     (P4, by design -- see RFC 0005) gets a no-op.
//   arm_from_autoseq_or_beacon/ignorelist_matches_normalized_dxcall: the
//     TX-arming trigger and its ignorelist filter. TX itself (RFC 0005 Phase 2
//     "headless decode/TX core") is a separate milestone from RX decode; a
//     board with no TX pipeline wired yet gets a no-op/pass-through.
//   porta_emit_decode: the sidekick link (RFC 0005 Phase 3 / B67). Safe to
//     call unconditionally — no-op until porta_start() has run; on P4 the
//     decode task and porta_tick() share the same image so emits reach the
//     phone once UAC is streaming.
//   ui_mark_rx_list_stale/ui_set_rx_list_static: components/ui's real
//     implementation REQUIRES the M5 display stack a headless board can't
//     link; each board defines these two directly instead.
extern std::string g_call;
extern volatile bool g_tx_view_dirty;
extern std::string g_last_reply_text;
extern bool g_rx_dirty;
void log_rxtx_line(char dir, int snr, int offset_hz, const std::string& text, int repeat_counter);
void arm_from_autoseq_or_beacon();
bool ignorelist_matches_normalized_dxcall(const std::string& dxcall_norm);
void porta_emit_decode(uint32_t decode_id, const char* text, int snr, int offset_hz,
                       float dt_s, bool is_cq, bool is_to_me, bool is_recent_qso);
void ui_mark_rx_list_stale();
void ui_set_rx_list_static(const RxDecodeEntry* entries, int count);

AdifLoggerDedupe s_adif_logger_dedupe;

// ---- Recent decodes, keyed by the id handed out with EVENT DECODE (docs/architecture/webapp.md (Screens)) so Port A's QUEUE_REPLY can name one without re-parsing rendered text
// on the phone. A ring, not a map: bounded and allocation-free like
// everything else on this path. An id that has scrolled out of the ring
// resolves to nothing, which porta_host_queue_reply reports plainly rather
// than guessing. ----
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
// directly to ui_set_rx_list_static without any conversion.
#define DEC_MAX       RX_MAX_DECODES       // 32
#define DEC_TEXT_MAX  RX_TEXT_MAX          // 64
#define DEC_FIELD_MAX RX_FIELD_MAX         // 20
typedef RxDecodeEntry DecodeMsg;

static DecodeMsg s_dec[DEC_MAX];
static int       s_dec_count;

// Plain-C field parser: tokenize text into field1/field2/field3.
static void dec_fill_fields(DecodeMsg* d) {
  d->field1[0] = d->field2[0] = d->field3[0] = '\0';
  char tmp[DEC_TEXT_MAX];
  strncpy(tmp, d->text, sizeof(tmp));
  tmp[sizeof(tmp) - 1] = '\0';

  char* saveptr = nullptr;
  char* toks[8];
  int ntoks = 0;
  for (char* p = strtok_r(tmp, " ", &saveptr); p && ntoks < 8; p = strtok_r(nullptr, " ", &saveptr)) {
    toks[ntoks++] = p;
  }
  if (ntoks == 0) return;

  auto all_digits = [](const char* s, int len) {
    for (int i = 0; i < len; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
  };
  auto all_alpha = [](const char* s, int len) {
    for (int i = 0; i < len; ++i) {
      char c = s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
  };

  // CQ <short_token> CALL GRID pattern
  if (strcmp(toks[0], "CQ") == 0 && ntoks >= 2) {
    int len1 = (int)strlen(toks[1]);
    bool short_tok = (len1 <= 3 && all_digits(toks[1], len1)) ||
                     (len1 <= 4 && all_alpha(toks[1], len1));
    if (short_tok) {
      strncpy(d->field1, toks[1], DEC_FIELD_MAX - 1); d->field1[DEC_FIELD_MAX - 1] = '\0';
      if (ntoks > 2) { strncpy(d->field2, toks[2], DEC_FIELD_MAX - 1); d->field2[DEC_FIELD_MAX - 1] = '\0'; }
      if (ntoks > 3) {
        d->field3[0] = '\0';
        for (int i = 3; i < ntoks; ++i) {
          if (i > 3) strncat(d->field3, " ", DEC_FIELD_MAX - strlen(d->field3) - 1);
          strncat(d->field3, toks[i], DEC_FIELD_MAX - strlen(d->field3) - 1);
        }
      }
      return;
    }
  }

  // Default: first 2 tokens + remainder
  strncpy(d->field1, toks[0], DEC_FIELD_MAX - 1); d->field1[DEC_FIELD_MAX - 1] = '\0';
  if (ntoks > 1) { strncpy(d->field2, toks[1], DEC_FIELD_MAX - 1); d->field2[DEC_FIELD_MAX - 1] = '\0'; }
  if (ntoks > 2) {
    d->field3[0] = '\0';
    for (int i = 2; i < ntoks; ++i) {
      if (i > 2) strncat(d->field3, " ", DEC_FIELD_MAX - strlen(d->field3) - 1);
      strncat(d->field3, toks[i], DEC_FIELD_MAX - strlen(d->field3) - 1);
    }
  }
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

// Marks the RX list stale in the UI component and flags core 0's dispatch
// loop to redraw it. Decode logic never draws directly -- drawing is the
// UI-consumer's job, triggered by the dirty flag it already polls.
static void keep_rx_list_stale() {
  ui_mark_rx_list_stale();
  g_rx_dirty = true;
}

void decode_monitor_results(monitor_t* mon, const monitor_config_t* cfg) {
  // One operator-log line per slot (B53 shape, slim). ESP_LOGD above is compiled
  // out at CONFIG_LOG_MAXIMUM_LEVEL=3; debug_log_line reaches P-then-. and the
  // browser. Cheap: two timestamps and counts already in hand — no extra decode work.
  const int64_t decode_t0_ms = rtc_now_ms();
  const int max_cand = 50;

  // ---- heap instrumentation ----
  size_t heap_entry = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t heap_entry_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  UBaseType_t stack_hw_entry = uxTaskGetStackHighWaterMark(NULL);
  ESP_LOGD(TAG, "DECODE_HEAP ENTER: free=%u largest=%u alltime_min=%u stack_hw=%u",
           (unsigned)heap_entry, (unsigned)heap_entry_largest,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned)stack_hw_entry);

  s_dec_count = 0;

  const int64_t now_ms = rtc_now_ms();
  static ftx_candidate_t candidates[max_cand];
  int num_candidates = ftx_find_candidates(&mon->wf, max_cand, candidates, 5);
  ESP_LOGD(TAG, "Candidates found: %d", num_candidates);

  // ---- slot index + once-per-slot hashtable maintenance ----
  int64_t slot_idx = (decode_tx_state_get_decode_slot_idx() >= 0) ? decode_tx_state_get_decode_slot_idx() : rtc_now_ms() / g_protocol->slot_time_ms;
  int slot_id = (int)(slot_idx & 1);

  static int64_t s_last_aged_slot = -1;
  if (slot_idx != s_last_aged_slot) {
    s_last_aged_slot = slot_idx;
    hashtable_age_all();
  }

  // ---- estimate noise floor ----
  float noise_db = -120.0f;
  if (mon->wf.mag && mon->wf.num_blocks > 0) {
    const size_t total = (size_t)mon->wf.num_blocks * (size_t)mon->wf.block_stride;
    static uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (size_t i = 0; i < total; ++i) hist[mon->wf.mag[i]]++;
    uint64_t target = total * 25 / 100;
    uint64_t accum = 0;
    int noise_scaled = 0;
    for (int v = 0; v < 256; ++v) {
      accum += hist[v];
      if (accum >= target) { noise_scaled = v; break; }
    }
    noise_db = 0.5f * ((float)noise_scaled - 240.0f);
  }

  // ---- mycall uppercase (stack, not heap) ----
  char mycall_up[DEC_FIELD_MAX];
  {
    const char* src = g_call.c_str();
    int len = (int)g_call.size();
    if (len >= DEC_FIELD_MAX) len = DEC_FIELD_MAX - 1;
    for (int i = 0; i < len; ++i) mycall_up[i] = (char)toupper((unsigned char)src[i]);
    mycall_up[len] = '\0';
  }

  // ---- decode candidates into static s_dec[] ----
  const int kMaxDecoded = 50;
  static ftx_message_t decoded[kMaxDecoded];
  static ftx_message_t* decoded_hashtable[kMaxDecoded];
  for (int i = 0; i < kMaxDecoded; ++i) decoded_hashtable[i] = nullptr;
  int num_decoded = 0;

  if (num_candidates <= 0) {
    ESP_LOGD(TAG, "No candidates found; keeping RX list");
    keep_rx_list_stale();
    // No candidates means we processed the slot's audio but found nothing —
    // still counts as "applied" for the TX-trigger guard.
    if (decode_tx_state_get_decode_slot_idx() > decode_tx_state_get_decode_applied_slot_idx()) {
      decode_tx_state_set_decode_applied_slot_idx(decode_tx_state_get_decode_slot_idx());
    }
    decode_tx_state_set_decode_in_progress(false);
    if (!decode_tx_state_get_was_txing()) {
      arm_from_autoseq_or_beacon();
      char buf[56];
      snprintf(buf, sizeof(buf), "SLOT cand=0/%d msg=0 dec=%lldms",
               max_cand, (long long)(rtc_now_ms() - decode_t0_ms));
      debug_log_line(buf);
    }
    return;
  }

  for (int i = 0; i < num_candidates && s_dec_count < DEC_MAX; ++i) {
    ftx_message_t message;
    ftx_decode_status_t status;
    memset(&message, 0, sizeof(message));
    memset(&status, 0, sizeof(status));

    if (!ftx_decode_candidate(&mon->wf, &candidates[i], 25, &message, &status))
      continue;

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
    if (found_dup || !found_empty) continue;

    memcpy(&decoded[idx_hash], &message, sizeof(message));
    decoded_hashtable[idx_hash] = &decoded[idx_hash];
    ++num_decoded;

    // decode to human text
    char text[FTX_MAX_MESSAGE_LENGTH] = {0};
    ftx_message_offsets_t offsets;
    ftx_message_rc_t urc = ftx_message_decode(&message, &hash_if, text, &offsets);
    if (urc != FTX_MESSAGE_RC_OK || text[0] == '\0') continue;

    // freq / time / SNR
    float freq_hz = (mon->min_bin + candidates[i].freq_offset +
                    candidates[i].freq_sub / (float)cfg->freq_osr) / mon->symbol_period;
    float time_s = (candidates[i].time_offset +
                   candidates[i].time_sub / (float)cfg->time_osr) * mon->symbol_period;

    float cand_db = noise_db;
    {
      int t_index = candidates[i].time_offset * mon->wf.time_osr + candidates[i].time_sub;
      const int t_count = mon->wf.num_blocks * mon->wf.time_osr;
      if (t_count > 0) { if (t_index < 0) t_index = 0; if (t_index >= t_count) t_index = t_count - 1; }
      else t_index = 0;

      int f_index = candidates[i].freq_sub * mon->wf.num_bins + candidates[i].freq_offset;
      const int f_count = mon->wf.freq_osr * mon->wf.num_bins;
      if (f_count > 0) { if (f_index < 0) f_index = 0; if (f_index >= f_count) f_index = f_count - 1; }
      else f_index = 0;

      size_t offset2 = (size_t)t_index * (size_t)f_count + (size_t)f_index;
      size_t total2 = (size_t)mon->wf.num_blocks * (size_t)mon->wf.block_stride;
      if (mon->wf.mag && offset2 < total2) cand_db = 0.5f * ((float)mon->wf.mag[offset2] - 240.0f);
    }

    int snr_q = (int)lrintf(cand_db - noise_db);
    if (snr_q < -30) snr_q = -30;
    if (snr_q >  99) snr_q = 99;

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

    dec_fill_fields(d);

    d->is_cq = (strncmp(d->text, "CQ ", 3) == 0 || strcmp(d->text, "CQ") == 0);

    char f1_norm[DEC_FIELD_MAX];
    dec_normalize_call(d->field1, f1_norm, DEC_FIELD_MAX);
    d->is_to_me = (mycall_up[0] != '\0' && strcmp(f1_norm, mycall_up) == 0);

    char dxnorm[DEC_FIELD_MAX];
    dec_normalize_call(d->field2, dxnorm, DEC_FIELD_MAX);
    d->is_recent_qso = (dxnorm[0] != '\0') &&
                       adif_logger_dedupe_is_duplicate(&s_adif_logger_dedupe, std::string(dxnorm), now_ms);

    log_rxtx_line('R', snr_q, (int)lrintf(freq_hz), std::string(final_text), -1);

    s_dec_count++;
  }

  ESP_LOGD(TAG, "Decoded %d unique messages", s_dec_count);

  // ---- Auto sync soft RTC from decode timing ----
  if (s_dec_count > 3) {
    // Simple insertion sort to find median of time_s values
    float sorted_t[DEC_MAX];
    int nt = 0;
    for (int i = 0; i < s_dec_count; ++i) sorted_t[nt++] = s_dec[i].time_s;
    for (int i = 1; i < nt; ++i) {
      float key = sorted_t[i];
      int j = i - 1;
      while (j >= 0 && sorted_t[j] > key) { sorted_t[j + 1] = sorted_t[j]; --j; }
      sorted_t[j + 1] = key;
    }
    float median = sorted_t[nt / 2];
    if (fabsf(median) > 0.3f) {
      int delta_ms = (int)lrintf(-median * 1000.0f);
      if (delta_ms > 320) delta_ms = 320;
      if (delta_ms < -320) delta_ms = -320;
      rtc_apply_decode_sync(delta_ms);
      ESP_LOGI("SYNC", "Applied RTC sync: median=%.2fs delta=%dms", median, delta_ms);
    }
  }

  // ---- Sort in-place: to_me, fresh CQ, other fresh, recent QSOs ----
  qsort(s_dec, s_dec_count, sizeof(DecodeMsg), dec_sort_cmp);

  // ---- Autoseq: build small to_me vector at boundary (only to_me entries) ----
  if (!decode_tx_state_get_was_txing() && !board_power_halted()) {
    std::vector<UiRxLine> to_me_auto;
    for (int i = 0; i < s_dec_count; ++i) {
      if (!s_dec[i].is_to_me) break;  // sorted, so once we pass to_me we're done
      char dxnorm[DEC_FIELD_MAX];
      dec_normalize_call(s_dec[i].field2, dxnorm, DEC_FIELD_MAX);
      if (ignorelist_matches_normalized_dxcall(std::string(dxnorm))) {
        ESP_LOGI(TAG, "IgnoreList: skip auto reply to %s", dxnorm);
        continue;
      }
      if (s_dec[i].is_recent_qso) {
        ESP_LOGI(TAG, "Recent QSO: skip auto reply to %s", dxnorm);
        continue;
      }
      UiRxLine rx;
      rx.text = s_dec[i].text;
      rx.field1 = s_dec[i].field1;
      rx.field2 = s_dec[i].field2;
      rx.field3 = s_dec[i].field3;
      rx.snr = s_dec[i].snr;
      rx.offset_hz = s_dec[i].offset_hz;
      rx.slot_id = s_dec[i].slot_id;
      rx.is_cq = s_dec[i].is_cq;
      rx.is_to_me = true;
      to_me_auto.push_back(std::move(rx));
    }

    if (!to_me_auto.empty()) {
      autoseq_on_decodes(to_me_auto);
      g_tx_view_dirty = true;
      g_last_reply_text = to_me_auto.front().text;
    }

    arm_from_autoseq_or_beacon();
  }

  // ---- Zero-heap handoff: static s_dec[] → ui.cpp's static rx_lines[] ----
  if (s_dec_count > 0) {
    ui_set_rx_list_static(s_dec, s_dec_count);
    // Same list, same moment, same order as the screen gets -- which is what
    // makes the on-screen log a free oracle for the link: if the browser and
    // the R screen disagree, the fault is between here and the browser.
    for (int i = 0; i < s_dec_count; ++i) {
      const RxDecodeEntry& e = s_dec[i];
      uint32_t decode_id = remember_decode_for_reply(e);
      porta_emit_decode(decode_id, e.text, e.snr, e.offset_hz, e.time_s,
                        e.is_cq, e.is_to_me, e.is_recent_qso);
    }
    g_rx_dirty = true;
  } else {
    ESP_LOGD(TAG, "No messages decoded; keeping RX list");
    keep_rx_list_stale();
  }

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

  if (!decode_tx_state_get_was_txing()) {
    char buf[56];
    snprintf(buf, sizeof(buf), "SLOT cand=%d/%d msg=%d dec=%lldms",
             num_candidates, max_cand, s_dec_count,
             (long long)(rtc_now_ms() - decode_t0_ms));
    debug_log_line(buf);
  }

  // Mark this slot's decode as fully applied BEFORE clearing the in-progress
  // flag. Readers (TX trigger on core 0) must see the applied marker as soon
  // as in_progress drops, not later.
  if (decode_tx_state_get_decode_slot_idx() > decode_tx_state_get_decode_applied_slot_idx()) {
    decode_tx_state_set_decode_applied_slot_idx(decode_tx_state_get_decode_slot_idx());
  }
  decode_tx_state_set_decode_in_progress(false);
}
