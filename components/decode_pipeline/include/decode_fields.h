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
