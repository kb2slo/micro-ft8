// cat_reply_parser: the replies the firmware reads back from a QMX
// (components/radio_control/include/cat_reply_parser.h).

#include <cstdio>
#include <cstring>

#include "cat_reply_parser.h"

static int g_fails = 0;

static void feed(cat_reply_parser_t* parser, const char* text)
{
    cat_reply_parser_feed(parser, reinterpret_cast<const uint8_t*>(text), strlen(text));
}

static void expect(int got, int want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, got, want);
        ++g_fails;
    }
}

int main()
{
    cat_reply_parser_t parser;
    cat_reply_parser_reset(&parser);
    expect(parser.sw_hundredths, -1, "nothing heard reads as none: SW");
    expect(parser.fa_hz, -1, "nothing heard reads as none: FA");
    expect(parser.pc_tenths, -1, "nothing heard reads as none: PC");
    expect(parser.sr_lock, -1, "nothing heard reads as none: SR");

    feed(&parser, "SW140;");
    expect(parser.sw_hundredths, 140, "SW reply is hundredths");
    feed(&parser, "PC45;");
    expect(parser.pc_tenths, 45, "PC reply is tenths of a watt");
    feed(&parser, "FA00014000000;");
    expect(parser.fa_hz, 14000000, "FA reply is the eleven-digit frequency");

    cat_reply_parser_reset(&parser);
    feed(&parser, "?;");
    feed(&parser, "SW;");
    feed(&parser, "ID020;");
    expect(parser.sw_hundredths, -1, "a refusal, an empty SW and other replies are ignored");
    expect(parser.fa_hz, -1, "and do not set FA");

    // SR is the SWR-protection lock: 1 locked, 0 clear. An empty SR is not a reading.
    cat_reply_parser_reset(&parser);
    feed(&parser, "SR1;");
    expect(parser.sr_lock, 1, "SR1 is locked");
    feed(&parser, "SR0;");
    expect(parser.sr_lock, 0, "SR0 is clear");
    cat_reply_parser_reset(&parser);
    feed(&parser, "SR;?;");
    expect(parser.sr_lock, -1, "an empty SR and a refusal are not a reading");

    // The radio appends terminal display codes after some replies; the next reply
    // must still parse, which it did not when they stayed in the frame.
    cat_reply_parser_reset(&parser);
    feed(&parser, "SW140;\x1b[7m\x1b[m\x1b[7m\x1b[m");
    expect(parser.sw_hundredths, 140, "display codes after a reply leave it intact");
    feed(&parser, "SW120;");
    expect(parser.sw_hundredths, 120, "and the next reply after them parses");
    feed(&parser, "\x1b[7;31mFA00007000000;");
    expect(parser.fa_hz, 7000000, "a code with parameters before a reply is skipped");
    feed(&parser, "\x1bXSW150;");
    expect(parser.sw_hundredths, 150, "a two-byte escape is skipped");

    // A reply split across two reads, as USB delivers them.
    cat_reply_parser_reset(&parser);
    feed(&parser, "FA0001");
    expect(parser.fa_hz, -1, "half a frame is not a reply");
    feed(&parser, "4225000;");
    expect(parser.fa_hz, 14225000, "a split reply parses when the rest arrives");

    // Junk that never ends must not overflow the frame, and must not block the next reply.
    cat_reply_parser_reset(&parser);
    char junk[200];
    memset(junk, 'x', sizeof(junk) - 1);
    junk[sizeof(junk) - 1] = 0;
    feed(&parser, junk);
    feed(&parser, ";SW133;");
    expect(parser.sw_hundredths, 133, "a long run of junk is dropped and the next reply parses");

    cat_reply_parser_reset(&parser);
    feed(&parser, "SW99999999999;");
    expect(parser.sw_hundredths, -1, "a value too big to be a reading is ignored");

    cat_reply_parser_reset(&parser);
    feed(&parser, "SW140;FA00014000000;PC45;");
    cat_reply_parser_reset(&parser);
    expect(parser.sw_hundredths, -1, "reset forgets SW");
    expect(parser.fa_hz, -1, "reset forgets FA");
    expect(parser.pc_tenths, -1, "reset forgets PC");

    if (g_fails == 0) {
        printf("PASS: cat_reply_parser\n");
    }
    return g_fails == 0 ? 0 : 1;
}
