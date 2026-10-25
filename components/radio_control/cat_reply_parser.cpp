#include "cat_reply_parser.h"

#include <string.h>

// A reply's digits: stops at the first non-digit, and -1 when there are none
// or the value is too big to be a real reading.
static int parse_digits(const char* digits)
{
    if (digits[0] < '0' || digits[0] > '9') {
        return -1;
    }
    long long value = 0;
    for (const char* p = digits; *p >= '0' && *p <= '9'; ++p) {
        value = value * 10 + (*p - '0');
        if (value > 2000000000LL) {
            return -1;
        }
    }
    return static_cast<int>(value);
}

// IF is the Kenwood TS-480 reply the QMX follows: after the two letters, the frequency in 11
// digits, then 4 step, 6 RIT, RIT on, XIT on, 1 bank, 2 channel, then TX/RX, mode, VFO, scan,
// split. Unchecked against a real QMX (docs/TEST_PLAN.md, Pending); a reply short of the split
// digit, or with a non-digit where a number belongs, is ignored whole.
static const size_t kIfFreqLen = 11;
static const size_t kIfTxAt = 26;
static const size_t kIfModeAt = 27;
static const size_t kIfSplitAt = 30;

static bool digit_at(const char* payload, size_t at, int* out)
{
    if (payload[at] < '0' || payload[at] > '9') {
        return false;
    }
    *out = payload[at] - '0';
    return true;
}

static void parse_if(cat_reply_parser_t* parser, const char* payload, size_t len)
{
    if (len < kIfSplitAt + 1) {
        return;
    }
    int hz = 0;
    for (size_t i = 0; i < kIfFreqLen; ++i) {
        int d = 0;
        if (!digit_at(payload, i, &d)) {
            return;
        }
        hz = hz * 10 + d;
        if (hz > 200000000) {
            return;
        }
    }
    int tx = 0, mode = 0, split = 0;
    if (!digit_at(payload, kIfTxAt, &tx) || !digit_at(payload, kIfModeAt, &mode) ||
        !digit_at(payload, kIfSplitAt, &split)) {
        return;
    }
    const bool changed = parser->if_hz != hz || parser->if_tx != tx || parser->if_mode != mode ||
                         parser->if_split != split;
    parser->if_tx = tx;
    parser->if_mode = mode;
    parser->if_split = split;
    parser->if_hz = hz;
    if (changed) {
        parser->if_changes = parser->if_changes + 1;
    }
}

static void parse_frame(cat_reply_parser_t* parser)
{
    const char* frame = parser->frame;
    if (frame[0] == 0 || frame[1] == 0) {
        return;
    }
    if (frame[0] == 'V' && frame[1] == 'N') {
        // "1_04_004": letters, digits, underscore and dot only, and short enough to be a version.
        const size_t len = parser->frame_len - 2;
        if (len == 0 || len >= sizeof(parser->vn)) {
            return;
        }
        for (size_t i = 0; i < len; ++i) {
            const char c = frame[2 + i];
            const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') || c == '_' || c == '.';
            if (!ok) {
                return;
            }
        }
        memcpy(parser->vn, frame + 2, len);
        parser->vn[len] = 0;
        return;
    }
    if (frame[0] == 'I' && frame[1] == 'F') {
        parse_if(parser, frame + 2, parser->frame_len - 2);
        return;
    }
    const int value = parse_digits(frame + 2);
    if (value < 0) {
        return;
    }
    if (frame[0] == 'P' && frame[1] == 'C') {
        parser->pc_tenths = value;
    } else if (frame[0] == 'S' && frame[1] == 'W') {
        parser->sw_hundredths = value;
    } else if (frame[0] == 'F' && frame[1] == 'A') {
        parser->fa_hz = value;
    } else if (frame[0] == 'S' && frame[1] == 'R') {
        parser->sr_lock = value;
    }
}

void cat_reply_parser_forget_version(cat_reply_parser_t* parser)
{
    parser->vn[0] = 0;
}

void cat_reply_parser_reset(cat_reply_parser_t* parser)
{
    parser->pc_tenths = -1;
    parser->sw_hundredths = -1;
    parser->fa_hz = -1;
    parser->sr_lock = -1;
    parser->if_hz = -1;
    parser->if_tx = -1;
    parser->if_mode = -1;
    parser->if_split = -1;
    parser->frame_len = 0;
    parser->escape_state = 0;
}

void cat_reply_parser_feed(cat_reply_parser_t* parser, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];
        switch (parser->escape_state) {
            case 1:
                // ESC [ opens a CSI sequence; any other byte ends a two-byte escape.
                parser->escape_state = (byte == '[') ? 2 : 0;
                continue;
            case 2:
                // Parameter and intermediate bytes (0x20-0x3F), then a final byte (0x40-0x7E).
                if (byte >= 0x40 && byte <= 0x7E) {
                    parser->escape_state = 0;
                }
                continue;
            default:
                break;
        }
        if (byte == 0x1b) {
            parser->escape_state = 1;
            continue;
        }
        if (byte == ';') {
            parser->frame[parser->frame_len] = 0;
            parse_frame(parser);
            parser->frame_len = 0;
            continue;
        }
        if (byte < 32 || byte > 126) {
            continue;
        }
        if (parser->frame_len + 1 < CAT_REPLY_FRAME_MAX) {
            parser->frame[parser->frame_len++] = static_cast<char>(byte);
        } else {
            parser->frame_len = 0;
        }
    }
}
