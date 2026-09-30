#include "porta_proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Same escaping host_link.c used when these rows were formatted inline.
// A quote or backslash is escaped; a control byte becomes \u00XX.
static void json_escape(const char *in, char *out, size_t out_cap)
{
    size_t w = 0;
    if (!in || out_cap == 0) {
        return;
    }
    for (size_t i = 0; in[i] && w + 7 < out_cap; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c < 0x20 || c == 0x7F) {
            w += (size_t)snprintf(&out[w], out_cap - w, "\\u%04x", c);
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

static size_t finish(char *out, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return (size_t)n;
}

size_t porta_event_json_log(char *out, size_t cap, uint32_t seq,
                            uint32_t epoch_secs, const char *text)
{
    char esc[PORTA_EVENT_TEXT_MAX * 6 + 1];
    json_escape(text ? text : "", esc, sizeof(esc));
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":0,\"x\":\"%s\"}",
                           seq, epoch_secs, esc);
    return finish(out, cap, n);
}

size_t porta_event_json_decode(char *out, size_t cap, uint32_t seq,
                               const porta_decode_event_t *ev)
{
    char esc[PORTA_EVENT_TEXT_MAX * 6 + 1];
    if (!ev) {
        return 0;
    }
    json_escape(ev->text, esc, sizeof(esc));
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":1,\"x\":\"%s\","
                           "\"id\":%" PRIu32 ",\"snr\":%d,\"hz\":%u,\"dt\":%.2f,\"cq\":%d,\"me\":%d,\"rq\":%d,"
                           "\"si\":%" PRIu32 ",\"sl\":%u,\"fq\":%" PRIu32 "}",
                           seq, ev->epoch_secs, esc, ev->decode_id, ev->snr,
                           ev->offset_hz, ev->dt_centis / 100.0, ev->is_cq ? 1 : 0,
                           ev->is_to_me ? 1 : 0, ev->is_recent_qso ? 1 : 0,
                           ev->slot_idx, (unsigned)ev->slot_len_ds, ev->dial_hz);
    return finish(out, cap, n);
}

size_t porta_event_json_queue(char *out, size_t cap, uint32_t seq,
                              const porta_queue_entry_event_t *ev)
{
    char esc[PORTA_CALLSIGN_MAX * 6 + 1];
    if (!ev) {
        return 0;
    }
    json_escape(ev->dxcall, esc, sizeof(esc));
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":2,\"eid\":%u,"
                           "\"st\":%u,\"rc\":%u,\"rl\":%u,\"x\":\"%s\"}",
                           seq, ev->epoch_secs, ev->entry_id, ev->state,
                           ev->retry_count, ev->retry_limit, esc);
    return finish(out, cap, n);
}

size_t porta_event_json_slot(char *out, size_t cap, uint32_t seq,
                             const porta_slot_state_event_t *ev)
{
    if (!ev) {
        return 0;
    }
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":3,"
                           "\"sp\":%u,\"bm\":%u,\"hz\":%u}",
                           seq, ev->epoch_secs, ev->slot_parity, ev->beacon_mode,
                           ev->resolved_offset_hz);
    return finish(out, cap, n);
}

size_t porta_event_json_tx_hud(char *out, size_t cap, uint32_t seq,
                               const porta_tx_hud_event_t *ev)
{
    char esc[PORTA_TX_HUD_TEXT_MAX * 6 + 1];
    char esc_reason[PORTA_TX_HUD_REASON_MAX * 6 + 1];
    if (!ev) {
        return 0;
    }
    json_escape(ev->text, esc, sizeof(esc));
    json_escape(ev->reason, esc_reason, sizeof(esc_reason));
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":4,"
                           "\"active\":%d,\"aborted\":%d,\"wrblk\":%d,"
                           "\"pw\":%d,\"swr\":%d,\"batt\":%d,\"hz\":%u,\"reason\":\"%s\",\"x\":\"%s\"}",
                           seq, ev->epoch_secs,
                           ev->active ? 1 : 0, ev->aborted ? 1 : 0, ev->writes_blocked ? 1 : 0,
                           ev->power_dw, ev->swr_c, ev->battery_pct, ev->offset_hz,
                           esc_reason, esc);
    return finish(out, cap, n);
}

size_t porta_event_json_usb(char *out, size_t cap, uint32_t seq,
                            const porta_usb_status_event_t *ev)
{
    if (!ev) {
        return 0;
    }
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":5,"
                           "\"present\":%d,\"kind\":%u,\"vid\":%u,\"pid\":%u}",
                           seq, ev->epoch_secs,
                           ev->present ? 1 : 0, ev->kind, ev->vid, ev->pid);
    return finish(out, cap, n);
}

size_t porta_event_json_sweep(char *out, size_t cap, uint32_t seq,
                              const porta_sweep_event_t *ev)
{
    if (!ev) {
        return 0;
    }
    const int n = snprintf(out, cap,
                           "{\"s\":%" PRIu32 ",\"t\":%" PRIu32 ",\"d\":6,"
                           "\"k\":%u,\"r\":%u,\"hz\":%" PRIu32 ",\"swr\":%d,\"why\":%u}",
                           seq, ev->epoch_secs, ev->kind, ev->range_index, ev->freq_hz,
                           ev->swr_c, ev->reason);
    return finish(out, cap, n);
}
