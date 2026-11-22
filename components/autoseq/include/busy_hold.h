#pragma once

#include <cctype>
#include <string>

// What one decode says of its sender, for the busy hold (docs/architecture/autoseq.md, The busy hold): whether
// the station is in a QSO with someone else, or free to answer us. Header only and pure, so host_mock pins it
// on its own; autoseq keeps each station's newest answer (autoseq_note_heard()).

enum class HeardKind : unsigned char {
    None,  // says nothing of its QSO state: free text, or not a standard message's shape
    Busy,  // continues a QSO with another call: a grid, a report, an R-report or a Field Day exchange
    Free,  // a CQ, a closing RR73, RRR or 73 to someone else, or a message to us
};

inline bool busy_hold_is_grid(const std::string& s) {
    return s.size() == 4 && isalpha((unsigned char)s[0]) && isalpha((unsigned char)s[1]) &&
           isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) && s != "RR73";
}

// "-12", "+05", "R-12", "R+05": what a report and an R-report look like.
inline bool busy_hold_is_report(const std::string& s) {
    size_t i = (!s.empty() && s[0] == 'R') ? 1 : 0;
    if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return false;
    ++i;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// A Field Day exchange after the two calls: "2A EMA", or "R 2A EMA". The class is a count of
// transmitters and a letter A to F.
inline bool busy_hold_is_fd_exchange(const std::string& s) {
    size_t i = (s.size() > 2 && s[0] == 'R' && s[1] == ' ') ? 2 : 0;
    const size_t digits = i;
    while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
    if (i == digits || i >= s.size() || s[i] < 'A' || s[i] > 'F') return false;
    ++i;
    return i < s.size() && s[i] == ' ';
}

inline bool busy_hold_looks_like_call(const std::string& s) {
    bool letter = false, digit = false;
    for (char c : s) {
        if (isalpha((unsigned char)c)) letter = true;
        else if (isdigit((unsigned char)c)) digit = true;
        else if (c != '/') return false;
    }
    return letter && digit && s.size() >= 3;
}

// `to` and `from` are the decode's field1 and field2 (decode_fields.h), upper case with <> stripped;
// `rest` its field3, upper case; `is_cq` whether it is a CQ (with any modifier). `my_call` is ours, the
// same way. A CQ or a message to us frees the station before anything else is looked at.
inline HeardKind busy_hold_kind(const std::string& to, const std::string& from, const std::string& rest,
                                bool is_cq, const std::string& my_call) {
    if (!busy_hold_looks_like_call(from)) return HeardKind::None;
    if (is_cq || to == "CQ") return HeardKind::Free;
    if (!busy_hold_looks_like_call(to)) return HeardKind::None;
    if (!my_call.empty() && to == my_call) return HeardKind::Free;
    if (rest == "RR73" || rest == "RRR" || rest == "73") return HeardKind::Free;
    if (busy_hold_is_grid(rest) || busy_hold_is_report(rest) || busy_hold_is_fd_exchange(rest)) {
        return HeardKind::Busy;
    }
    return HeardKind::None;
}
