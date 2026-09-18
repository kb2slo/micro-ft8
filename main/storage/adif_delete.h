#pragma once

#include <string>
#include <vector>

// Port A / phone Log delete of daily QSO archives (YYYYMMDD.adi and legacy
// .txt day files — the same set FILE_LIST / qso_browse expose).
//
// `filename` empty → clear every daily file that `is_locked` does not protect.
// Non-empty → delete that one basename (must be a daily log name).
//
// `is_locked` matches on-device D-screen LOCK: during TX/decode/streaming the
// active day file (and RT log / fieldday on ADV) must not be removed.
// Returns nullptr on success, else a short static reason for ACK/NAK.

using AdifDeleteIsLockedFn = bool (*)(const char* basename);

const char* adif_delete_daily(const char* filename, AdifDeleteIsLockedFn is_locked);

// Pure planner for host tests: fills `to_delete` from `all_names` without
// touching storage. Same return contract as adif_delete_daily.
const char* adif_delete_plan(const char* filename,
                             const std::vector<std::string>& all_names,
                             AdifDeleteIsLockedFn is_locked,
                             std::vector<std::string>* to_delete);
