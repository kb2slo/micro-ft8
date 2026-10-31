/*
 * decode_fields_fill() (decode_fields.h): a decode's text split into the
 * fields autoseq and the heard-grid table read. Pins that field2 is the sender
 * and field3 what it sent, with or without a CQ modifier. Also
 * decode_dxpedition_rewrite(): a fox's two-hound message reduced to our half.
 */
#include <cstdio>
#include <cstring>

#include "decode_fields.h"

static int s_failed = 0;

static void expect(const char* text, const char* f1, const char* f2, const char* f3)
{
    RxDecodeEntry d = {};
    snprintf(d.text, sizeof(d.text), "%s", text);
    decode_fields_fill(&d);
    if (strcmp(d.field1, f1) != 0 || strcmp(d.field2, f2) != 0 || strcmp(d.field3, f3) != 0) {
        fprintf(stderr, "FAIL: \"%s\" -> [%s|%s|%s], expected [%s|%s|%s]\n", text, d.field1,
                d.field2, d.field3, f1, f2, f3);
        s_failed = 1;
    }
}

static void expect_fox(const char* text, const char* me, const char* want)
{
    char out[RX_TEXT_MAX] = "untouched";
    const bool got = decode_dxpedition_rewrite(text, me, out, sizeof(out));
    const bool rewrites = want != nullptr;
    if (got != rewrites || strcmp(out, rewrites ? want : "untouched") != 0) {
        fprintf(stderr, "FAIL: fox \"%s\" as %s -> %d \"%s\", expected %s\n", text, me, got, out,
                rewrites ? want : "no rewrite");
        s_failed = 1;
    }
}

int main()
{
    // CQ, plain and with a modifier: the sender and its grid land in field2
    // and field3 either way.
    expect("CQ LW6EMN GF03", "CQ", "LW6EMN", "GF03");
    expect("CQ DX LW6EMN GF03", "DX", "LW6EMN", "GF03");
    expect("CQ POTA K1ABC FN42", "POTA", "K1ABC", "FN42");
    expect("CQ SOTA K1ABC FN42", "SOTA", "K1ABC", "FN42");
    expect("CQ NA W1AW FN31", "NA", "W1AW", "FN31");
    expect("CQ 123 W1AW FN31", "123", "W1AW", "FN31");
    expect("CQ W1AW", "CQ", "W1AW", "");
    expect("CQ DX W1AW", "DX", "W1AW", "");
    expect("CQ", "CQ", "", "");

    // A 4-letter modifier is the limit; a callsign is never all letters or
    // all digits, so it is never taken for one.
    expect("CQ WWFF K1ABC FN42", "WWFF", "K1ABC", "FN42");
    expect("CQ K1A FN42", "CQ", "K1A", "FN42");

    // Messages between two stations: to, from, then the rest.
    expect("W1AW LW6EMN GF03", "W1AW", "LW6EMN", "GF03");
    expect("KB2SLO LW6EMN -13", "KB2SLO", "LW6EMN", "-13");
    expect("KB2SLO LW6EMN R-13", "KB2SLO", "LW6EMN", "R-13");
    expect("W9XYZ K1ABC RR73", "W9XYZ", "K1ABC", "RR73");
    expect("W9XYZ K1ABC R FN42", "W9XYZ", "K1ABC", "R FN42");
    expect("<PJ4/K1ABC> W1AW RR73", "<PJ4/K1ABC>", "W1AW", "RR73");
    expect("TNX BOB 73 GL", "TNX", "BOB", "73 GL");
    expect("", "", "", "");

    // Fox messages: the half meant for us, as the fox printed the calls.
    expect_fox("K1ABC RR73; W9XYZ <KH1/KH7Z> -12", "K1ABC", "K1ABC <KH1/KH7Z> RR73");
    expect_fox("K1ABC RR73; W9XYZ <KH1/KH7Z> -12", "W9XYZ", "W9XYZ <KH1/KH7Z> -12");
    expect_fox("<K1ABC> RR73; W9XYZ KH1 -05", "K1ABC", "<K1ABC> KH1 RR73");
    expect_fox("k1abc RR73; W9XYZ KH1 -05", "K1ABC", "k1abc KH1 RR73");
    expect_fox("K1ABC  RR73;  W9XYZ KH1 -05", "K1ABC", "K1ABC KH1 RR73");
    // Not for us, not a fox message, or no call set: no rewrite.
    expect_fox("K1ABC RR73; W9XYZ KH1 -05", "N0CALL", nullptr);
    expect_fox("K1ABC RR73; W9XYZ KH1 -05", "", nullptr);
    expect_fox("K1ABC RR73 W9XYZ KH1 -05", "K1ABC", nullptr);
    expect_fox("K1ABC RR73; W9XYZ KH1", "K1ABC", nullptr);
    expect_fox("K1ABC RR73; W9XYZ KH1 -05 X", "K1ABC", nullptr);
    expect_fox("<> RR73; K1ABC KH1 +03", "K1ABC", nullptr);
    expect_fox("CQ K1ABC FN42", "K1ABC", nullptr);

    if (s_failed) return 1;
    printf("PASS: decode fields: field2 is the sender, with or without a CQ modifier; "
           "a fox message reduces to our half\n");
    return 0;
}
