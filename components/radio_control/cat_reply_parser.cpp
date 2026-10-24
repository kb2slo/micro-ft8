#include "cat_reply_parser.h"

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

static void parse_frame(cat_reply_parser_t* parser)
{
    const char* frame = parser->frame;
    if (frame[0] == 0 || frame[1] == 0) {
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

void cat_reply_parser_reset(cat_reply_parser_t* parser)
{
    parser->pc_tenths = -1;
    parser->sw_hundredths = -1;
    parser->fa_hz = -1;
    parser->sr_lock = -1;
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
