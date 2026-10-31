#pragma once

#include "decode_types.h"

// Splits d->text into field1, field2 and field3, which autoseq, the decode
// list and the heard-grid table read. field2 is always the sender and field3
// what follows it (a grid, a report, RR73; several words are kept together):
//   W1AW K1ABC FN42     -> W1AW  / K1ABC / FN42
//   CQ K1ABC FN42       -> CQ    / K1ABC / FN42
//   CQ DX K1ABC FN42    -> DX    / K1ABC / FN42   (a modifier of up to 3 digits
//                                                    or 4 letters takes CQ's place)
// A callsign always mixes letters and digits, so it is never taken for a modifier.
void decode_fields_fill(RxDecodeEntry* d);

// A DXpedition (fox) message, "CALL1 RR73; CALL2 FOX RPT", answers two hounds at
// once. When one of them is mycall_up (upper case), writes the half meant for
// us to out, as a plain message autoseq can sequence:
//   CALL1 is us -> "CALL1 FOX RR73"
//   CALL2 is us -> "CALL2 FOX RPT"
// <> around a hashed call and case are ignored when comparing. Returns false,
// leaving out alone, for any other text or an empty mycall_up.
bool decode_dxpedition_rewrite(const char* text, const char* mycall_up, char* out, size_t out_len);
