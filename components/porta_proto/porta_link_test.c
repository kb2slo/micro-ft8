#include <stdio.h>
#include <string.h>

#include "porta_proto.h"

// Index 0 is the resting rate. The rest are what the diagnostic can switch
// to, not a climb. Adding one means both ends flash together; a peer that
// does not know it refuses the set-rate frame.
static const uint32_t kRates[] = {
    PORTA_BAUD,
    115200u,
    230400u,
    460800u,
    921600u,
    1500000u,
    2000000u,
    // 80 MHz / 20. The P4 held a 100 KB run (2026-10-01); the sidekick did
    // not. Not the pass.
    4000000u,
};

// Short quotas for a tuning run, then the final pass. Each short one is a
// whole number of max-length frames. 14 MB is not, and the run sends one
// more frame to cover it.
static const uint32_t kPassBytes[] = {
    10200u,
    102000u,
    1020000u,
    PORTA_LINK_PASS_BYTES,
};

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// One byte, independent of every other byte, so a missing frame does not
// change what a later frame must contain. The added constant keeps a zero
// seed from collapsing the mix to zero.
static uint8_t prng_byte(uint32_t seed, uint32_t seq, uint32_t index)
{
    uint32_t x = seed + seq * 0x9E3779B9u + index * 0x85EBCA6Bu + 0xA5A5A5A5u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (uint8_t)x;
}

static uint8_t pattern_byte(uint32_t pattern, uint32_t seed, uint32_t seq,
                            uint32_t index)
{
    switch (pattern) {
    case PORTA_LINK_PAT_SYNC:
        return PORTA_PROTO_SYNC;
    case PORTA_LINK_PAT_ZERO:
        return 0x00u;
    case PORTA_LINK_PAT_ONES:
        return 0xFFu;
    case PORTA_LINK_PAT_ALT:
        return (index & 1u) ? 0x55u : 0xAAu;
    case PORTA_LINK_PAT_PRNG:
        return prng_byte(seed, seq, index);
    default:
        return 0u;
    }
}

void porta_link_data_fill(uint8_t payload[PORTA_PROTO_MAX_PAYLOAD],
                          uint32_t seed, uint32_t seq)
{
    if (!payload) {
        return;
    }
    put_u32(payload, seq);
    const uint32_t pattern = seq % (uint32_t)PORTA_LINK_PAT_COUNT;
    for (uint32_t i = 4; i < PORTA_PROTO_MAX_PAYLOAD; ++i) {
        payload[i] = pattern_byte(pattern, seed, seq, i);
    }
}

void porta_link_checker_init(porta_link_checker_t *c, uint32_t seed)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->seed = seed;
}

bool porta_link_checker_push(porta_link_checker_t *c, const uint8_t *payload,
                             uint8_t len)
{
    if (!c || !payload || len != PORTA_PROTO_MAX_PAYLOAD) {
        return false;
    }

    const uint32_t seq = get_u32(payload);
    uint8_t expect[PORTA_PROTO_MAX_PAYLOAD];
    porta_link_data_fill(expect, c->seed, seq);
    const bool bytes_ok = memcmp(payload, expect, PORTA_PROTO_MAX_PAYLOAD) == 0;

    if (seq < c->next_seq) {
        c->duplicates++;
        if (!bytes_ok) {
            c->mismatches++;
        }
        return true;
    }
    if (seq > c->next_seq) {
        c->drops += seq - c->next_seq;
    }
    if (bytes_ok) {
        c->frames_ok++;
    } else {
        c->mismatches++;
    }
    c->next_seq = seq + 1u;
    return true;
}

unsigned porta_link_rate_count(void)
{
    return (unsigned)(sizeof(kRates) / sizeof(kRates[0]));
}

uint32_t porta_link_rate_at(unsigned index)
{
    if (index >= porta_link_rate_count()) {
        return PORTA_BAUD;
    }
    return kRates[index];
}

bool porta_link_rate_allowed(uint32_t baud)
{
    for (unsigned i = 0; i < porta_link_rate_count(); ++i) {
        if (kRates[i] == baud) {
            return true;
        }
    }
    return false;
}

unsigned porta_link_pass_count(void)
{
    return (unsigned)(sizeof(kPassBytes) / sizeof(kPassBytes[0]));
}

uint32_t porta_link_pass_at(unsigned index)
{
    if (index >= porta_link_pass_count()) {
        return 0;
    }
    return kPassBytes[index];
}

bool porta_link_pass_allowed(uint32_t bytes)
{
    for (unsigned i = 0; i < porta_link_pass_count(); ++i) {
        if (kPassBytes[i] == bytes) {
            return true;
        }
    }
    return false;
}

uint32_t porta_link_baud_now(uint32_t requested_baud, uint32_t now_ms,
                            uint32_t last_data_ms)
{
    if (!porta_link_rate_allowed(requested_baud) || requested_baud == PORTA_BAUD) {
        return PORTA_BAUD;
    }
    if ((now_ms - last_data_ms) >= PORTA_LINK_IDLE_MS) {
        return PORTA_BAUD;
    }
    return requested_baud;
}

static size_t encode_cmd(uint8_t cmd, const uint8_t *rest, uint8_t rest_len,
                         uint8_t *out, size_t out_cap)
{
    uint8_t payload[32];
    if (rest_len > sizeof(payload) - 1u) {
        return 0;
    }
    payload[0] = cmd;
    if (rest_len > 0 && rest) {
        memcpy(&payload[1], rest, rest_len);
    }
    return porta_proto_encode(PORTA_MSG_LINK_TEST, payload, (uint8_t)(1u + rest_len),
                              out, out_cap);
}

size_t porta_proto_encode_link_start(uint32_t seed, bool tune, uint32_t pass_bytes,
                                     uint8_t *out, size_t out_cap)
{
    if (!porta_link_pass_allowed(pass_bytes)) {
        return 0;
    }
    uint8_t arg[9];
    put_u32(arg, seed);
    arg[4] = tune ? 1u : 0u;
    put_u32(&arg[5], pass_bytes);
    return encode_cmd(PORTA_LINK_START, arg, 9, out, out_cap);
}

size_t porta_proto_encode_link_set_rate(uint32_t baud, uint8_t *out, size_t out_cap)
{
    if (!porta_link_rate_allowed(baud)) {
        return 0;
    }
    uint8_t arg[4];
    put_u32(arg, baud);
    return encode_cmd(PORTA_LINK_SET_RATE, arg, 4, out, out_cap);
}

size_t porta_proto_encode_link_stop(uint8_t *out, size_t out_cap)
{
    return encode_cmd(PORTA_LINK_STOP, NULL, 0, out, out_cap);
}

size_t porta_proto_encode_link_data(uint32_t seed, uint32_t seq,
                                   uint8_t *out, size_t out_cap)
{
    uint8_t payload[PORTA_PROTO_MAX_PAYLOAD];
    porta_link_data_fill(payload, seed, seq);
    return porta_proto_encode(PORTA_MSG_LINK_DATA, payload, PORTA_PROTO_MAX_PAYLOAD,
                              out, out_cap);
}

static bool parse_cmd(const porta_frame_t *f, uint8_t cmd, uint8_t expect_len,
                      uint32_t *arg_out)
{
    if (!f || f->type != PORTA_MSG_LINK_TEST || f->len != expect_len ||
        f->payload[0] != cmd) {
        return false;
    }
    if (arg_out) {
        *arg_out = (expect_len == 5) ? get_u32(&f->payload[1]) : 0u;
    }
    return true;
}

bool porta_proto_parse_link_start(const porta_frame_t *f, uint32_t *seed_out,
                                  bool *tune_out, uint32_t *pass_bytes_out)
{
    if (!f || f->type != PORTA_MSG_LINK_TEST || f->len != 10 ||
        f->payload[0] != PORTA_LINK_START) {
        return false;
    }
    const uint32_t bytes = get_u32(&f->payload[6]);
    if (!porta_link_pass_allowed(bytes)) {
        return false;
    }
    if (seed_out) {
        *seed_out = get_u32(&f->payload[1]);
    }
    if (tune_out) {
        *tune_out = (f->payload[5] != 0);
    }
    if (pass_bytes_out) {
        *pass_bytes_out = bytes;
    }
    return true;
}

bool porta_proto_parse_link_set_rate(const porta_frame_t *f, uint32_t *baud_out)
{
    uint32_t baud = 0;
    if (!parse_cmd(f, PORTA_LINK_SET_RATE, 5, &baud) || !porta_link_rate_allowed(baud)) {
        return false;
    }
    if (baud_out) {
        *baud_out = baud;
    }
    return true;
}

bool porta_proto_parse_link_stop(const porta_frame_t *f)
{
    return parse_cmd(f, PORTA_LINK_STOP, 1, NULL);
}

static void put_status(uint8_t *p, const porta_link_status_t *s)
{
    put_u32(&p[0], s->baud);
    put_u32(&p[4], s->frames_ok);
    put_u32(&p[8], s->drops);
    put_u32(&p[12], s->duplicates);
    put_u32(&p[16], s->mismatches);
    put_u32(&p[20], s->crc_errors);
    put_u32(&p[24], s->tx_bytes);
    p[28] = (uint8_t)((s->running ? 1u : 0u) | (s->failed ? 2u : 0u) |
                      (s->passed ? 4u : 0u) | (s->tune ? 8u : 0u));
}

size_t porta_proto_encode_link_result(const porta_link_status_t *status,
                                     uint8_t *out, size_t out_cap)
{
    if (!status) {
        return 0;
    }
    uint8_t rest[29];
    put_status(rest, status);
    return encode_cmd(PORTA_LINK_RESULT, rest, 29, out, out_cap);
}

bool porta_proto_parse_link_result(const porta_frame_t *f, porta_link_status_t *out)
{
    if (!f || !out || f->type != PORTA_MSG_LINK_TEST || f->len != 30 ||
        f->payload[0] != PORTA_LINK_RESULT) {
        return false;
    }
    const uint8_t *p = &f->payload[1];
    memset(out, 0, sizeof(*out));
    out->baud = get_u32(&p[0]);
    out->frames_ok = get_u32(&p[4]);
    out->drops = get_u32(&p[8]);
    out->duplicates = get_u32(&p[12]);
    out->mismatches = get_u32(&p[16]);
    out->crc_errors = get_u32(&p[20]);
    out->tx_bytes = get_u32(&p[24]);
    out->running = (p[28] & 1u) != 0;
    out->failed = (p[28] & 2u) != 0;
    out->passed = (p[28] & 4u) != 0;
    out->tune = (p[28] & 8u) != 0;
    return true;
}

static void reset_rate(porta_link_run_t *r, uint32_t now_ms, uint32_t crc_errors)
{
    porta_link_checker_init(&r->rx, r->seed);
    r->tx_seq = 0;
    r->tx_bytes = 0;
    r->crc_base = crc_errors;
    r->last_data_ms = now_ms;
    r->last_rx_ms = now_ms;
    r->last_result_ms = now_ms;
    r->rate_failed = false;
    r->crc_latched = true;
}

static void write_note(porta_link_run_t *r, bool ok, uint32_t crc_errors)
{
    const uint32_t crc_delta = crc_errors - r->crc_base;
    snprintf(r->note, sizeof(r->note), "link %lu %s d=%lu m=%lu c=%lu",
             (unsigned long)r->baud, ok ? "ok" : "fail",
             (unsigned long)r->rx.drops, (unsigned long)r->rx.mismatches,
             (unsigned long)crc_delta);
    r->note_ready = true;
}

static bool rate_clean(const porta_link_run_t *r, uint32_t crc_errors)
{
    return r->rx.drops == 0 && r->rx.duplicates == 0 && r->rx.mismatches == 0 &&
           crc_errors == r->crc_base &&
           r->rx.frames_ok * (uint32_t)PORTA_PROTO_MAX_PAYLOAD >= r->pass_bytes;
}

static bool tx_done(const porta_link_run_t *r)
{
    return r->tx_bytes >= r->pass_bytes;
}

// The peer's RESULT for this baud: it has received a full quota and counted
// no damage. The coordinator does not raise the rate on its own receive alone.
static bool peer_rx_clean(const porta_link_run_t *r)
{
    if (!r->peer_valid || r->peer.baud != r->baud) {
        return false;
    }
    return r->peer.drops == 0 && r->peer.duplicates == 0 && r->peer.mismatches == 0 &&
           r->peer.crc_errors == 0 &&
           r->peer.frames_ok * (uint32_t)PORTA_PROTO_MAX_PAYLOAD >= r->pass_bytes;
}

void porta_link_run_init(porta_link_run_t *r)
{
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->baud = PORTA_BAUD;
}

void porta_link_run_begin(porta_link_run_t *r, uint32_t seed, uint32_t pass_bytes,
                          bool coordinator, bool tune, unsigned last_rate,
                          uint32_t now_ms)
{
    if (!r || pass_bytes == 0) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->phase = PORTA_LINK_PHASE_RUN;
    r->coordinator = coordinator;
    r->tune = tune;
    r->seed = seed;
    r->pass_bytes = pass_bytes;
    r->baud = PORTA_BAUD;
    r->need_start = coordinator;
    r->rate_index = 0;
    r->rate_last = last_rate;
    if (r->rate_last >= porta_link_rate_count()) {
        r->rate_last = porta_link_rate_count() - 1u;
    }
    // START goes out at the resting rate. The quota runs only at rate_last.
    r->need_rate = coordinator && r->rate_last != 0;
    reset_rate(r, now_ms, 0);
    r->crc_latched = false;
}

void porta_link_run_stop(porta_link_run_t *r)
{
    if (!porta_link_run_active(r)) {
        return;
    }
    r->stop_requested = true;
}

bool porta_link_run_active(const porta_link_run_t *r)
{
    if (!r) {
        return false;
    }
    return r->phase == PORTA_LINK_PHASE_RUN || r->phase == PORTA_LINK_PHASE_SETTLE || r->hold_tx;
}

void porta_link_run_applied(porta_link_run_t *r, uint32_t now_ms)
{
    if (!r || !r->hold_tx) {
        return;
    }
    r->baud = r->pending_baud;
    r->hold_tx = false;
    r->phase = PORTA_LINK_PHASE_SETTLE;
    // The peer switches when the set-rate frame arrives, which is before the
    // coordinator finishes flushing it. Skip the peer's wait so it is already
    // listening when the coordinator's settle ends and the first frame goes out.
    r->settle_at_ms = r->coordinator ? now_ms : now_ms - PORTA_LINK_SETTLE_MS;
}

static void enter_done(porta_link_run_t *r, bool ok, uint32_t crc_errors)
{
    write_note(r, ok, crc_errors);
    r->phase = PORTA_LINK_PHASE_DONE;
    r->hold_tx = false;
    r->sweep_passed = ok && !r->stop_requested;
    r->rate_failed = !ok;
}

size_t porta_link_run_next(porta_link_run_t *r, uint32_t now_ms, uint32_t crc_errors,
                           uint8_t *out, size_t out_cap, uint32_t *apply_baud)
{
    if (apply_baud) {
        *apply_baud = 0;
    }
    if (!r || !out || r->phase == PORTA_LINK_PHASE_IDLE || r->phase == PORTA_LINK_PHASE_DONE) {
        return 0;
    }
    if (r->stop_requested) {
        r->hold_tx = false;
        r->phase = PORTA_LINK_PHASE_DONE;
        r->sweep_passed = false;
        if (r->coordinator && !r->stop_sent) {
            r->stop_sent = true;
            write_note(r, false, crc_errors);
            return porta_proto_encode_link_stop(out, out_cap);
        }
        return 0;
    }
    if (r->hold_tx) {
        return 0;
    }
    if (r->phase == PORTA_LINK_PHASE_SETTLE) {
        if ((now_ms - r->settle_at_ms) < PORTA_LINK_SETTLE_MS) {
            return 0;
        }
        r->phase = PORTA_LINK_PHASE_RUN;
        reset_rate(r, now_ms, crc_errors);
        if (!r->coordinator) {
            r->await_coord = true;
        }
    }
    if (!r->crc_latched) {
        r->crc_base = crc_errors;
        r->crc_latched = true;
    }

    const uint32_t held = porta_link_baud_now(r->baud, now_ms, r->last_data_ms);
    if (held != r->baud) {
        enter_done(r, false, crc_errors);
        r->baud = PORTA_BAUD;
        if (r->coordinator && !r->stop_sent) {
            r->stop_sent = true;
            return porta_proto_encode_link_stop(out, out_cap);
        }
        return 0;
    }

    if (r->need_start) {
        r->need_start = false;
        return porta_proto_encode_link_start(r->seed, r->tune, r->pass_bytes, out, out_cap);
    }

    if (r->need_rate) {
        r->need_rate = false;
        const uint32_t baud = porta_link_rate_at(r->rate_last);
        r->pending_baud = baud;
        r->rate_index = r->rate_last;
        r->hold_tx = true;
        if (apply_baud) {
            *apply_baud = baud;
        }
        return porta_proto_encode_link_set_rate(baud, out, out_cap);
    }

    if (r->await_coord) {
        return 0;
    }

    if (tx_done(r)) {
        const bool clean = rate_clean(r, crc_errors);
        const bool bad = r->rx.drops || r->rx.duplicates || r->rx.mismatches ||
                         crc_errors != r->crc_base;
        const bool stalled = (now_ms - r->last_rx_ms) >= PORTA_LINK_IDLE_MS;
        const bool peer_bad = r->peer_valid && r->peer.baud == r->baud &&
                              (r->peer.drops || r->peer.duplicates || r->peer.mismatches ||
                               r->peer.crc_errors);
        if (!r->coordinator) {
            if (!clean && (bad || stalled)) {
                enter_done(r, false, crc_errors);
                return 0;
            }
            // Keep reporting after our own quota. Otherwise the coordinator
            // never sees that this direction finished clean.
            if ((now_ms - r->last_result_ms) >= 1000u) {
                porta_link_status_t st;
                porta_link_run_status(r, crc_errors, &st);
                r->last_result_ms = now_ms;
                return porta_proto_encode_link_result(&st, out, out_cap);
            }
            return 0;
        }
        if ((!clean && (bad || stalled)) || peer_bad || (clean && stalled && !peer_rx_clean(r))) {
            enter_done(r, false, crc_errors);
            if (!r->stop_sent) {
                r->stop_sent = true;
                return porta_proto_encode_link_stop(out, out_cap);
            }
            return 0;
        }
        if (clean && peer_rx_clean(r)) {
            enter_done(r, true, crc_errors);
            if (!r->stop_sent) {
                r->stop_sent = true;
                return porta_proto_encode_link_stop(out, out_cap);
            }
            return 0;
        }
        return 0;
    }

    if (!r->coordinator && (now_ms - r->last_result_ms) >= 1000u) {
        porta_link_status_t st;
        porta_link_run_status(r, crc_errors, &st);
        r->last_result_ms = now_ms;
        return porta_proto_encode_link_result(&st, out, out_cap);
    }

    const size_t n = porta_proto_encode_link_data(r->seed, r->tx_seq, out, out_cap);
    if (n == 0) {
        return 0;
    }
    r->tx_seq++;
    r->tx_bytes += PORTA_PROTO_MAX_PAYLOAD;
    r->last_data_ms = now_ms;
    return n;
}

void porta_link_run_on_frame(porta_link_run_t *r, const porta_frame_t *f,
                             uint32_t now_ms, uint32_t crc_errors, uint32_t *switch_baud)
{
    if (switch_baud) {
        *switch_baud = 0;
    }
    if (!r || !f) {
        return;
    }
    if (f->type == PORTA_MSG_LINK_DATA) {
        if (r->phase == PORTA_LINK_PHASE_RUN && !r->hold_tx) {
            r->await_coord = false;
            porta_link_checker_push(&r->rx, f->payload, f->len);
            r->last_rx_ms = now_ms;
            r->last_data_ms = now_ms;
        }
        return;
    }
    if (f->type != PORTA_MSG_LINK_TEST) {
        return;
    }
    uint32_t seed = 0;
    bool tune = false;
    uint32_t pass_bytes = 0;
    if (porta_proto_parse_link_start(f, &seed, &tune, &pass_bytes)) {
        // DONE as well as IDLE. A finished run stays DONE, and a later Start
        // has to begin again. Ignoring it leaves the coordinator sending into
        // silence: tx reaches the quota, ok stays 0, and the run fails.
        if (!r->coordinator && !porta_link_run_active(r)) {
            porta_link_run_begin(r, seed, pass_bytes, false, tune, 0, now_ms);
            r->crc_base = crc_errors;
        }
        return;
    }
    uint32_t baud = 0;
    if (porta_proto_parse_link_set_rate(f, &baud)) {
        if (!r->coordinator && porta_link_run_active(r)) {
            int index = -1;
            for (unsigned i = 0; i < porta_link_rate_count(); ++i) {
                if (porta_link_rate_at(i) == baud) {
                    index = (int)i;
                }
            }
            if (index < 0) {
                return;
            }
            write_note(r, rate_clean(r, crc_errors), crc_errors);
            r->pending_baud = baud;
            r->rate_index = (unsigned)index;
            r->hold_tx = true;
            if (switch_baud) {
                *switch_baud = baud;
            }
        }
        return;
    }
    if (porta_proto_parse_link_stop(f)) {
        if (porta_link_run_active(r)) {
            const bool ok = rate_clean(r, crc_errors) && !r->rate_failed;
            enter_done(r, ok, crc_errors);
        }
        return;
    }
    porta_link_status_t peer;
    if (porta_proto_parse_link_result(f, &peer)) {
        r->peer = peer;
        r->peer_valid = true;
    }
}

void porta_link_run_status(const porta_link_run_t *r, uint32_t crc_errors,
                           porta_link_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!r || r->phase == PORTA_LINK_PHASE_IDLE) {
        out->baud = PORTA_BAUD;
        return;
    }
    out->baud = r->baud;
    out->frames_ok = r->rx.frames_ok;
    out->drops = r->rx.drops;
    out->duplicates = r->rx.duplicates;
    out->mismatches = r->rx.mismatches;
    out->crc_errors = crc_errors - r->crc_base;
    out->tx_bytes = r->tx_bytes;
    out->running = porta_link_run_active(r);
    out->failed = r->rate_failed || (r->phase == PORTA_LINK_PHASE_DONE && !r->sweep_passed);
    out->passed = r->sweep_passed;
    out->tune = r->tune;
}

size_t porta_link_run_take_note(porta_link_run_t *r, char *out, size_t cap)
{
    if (!r || !out || cap == 0 || !r->note_ready) {
        return 0;
    }
    r->note_ready = false;
    const size_t n = strlen(r->note);
    if (n + 1u > cap) {
        return 0;
    }
    memcpy(out, r->note, n + 1u);
    return n;
}
