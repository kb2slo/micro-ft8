#include "dial_history.h"

namespace {

struct Tune {
    int64_t at_ms;
    uint32_t hz;
};

// A ring, oldest first from s_head. Not locked: it is noted from the CAT path
// and read as decodes are emitted, both on the main loop
// (docs/architecture/main_board.md, P4).
Tune s_tunes[DIAL_HISTORY_LEN];
int s_head = 0;
int s_count = 0;

const Tune& nth(int i) { return s_tunes[(s_head + i) % DIAL_HISTORY_LEN]; }

}  // namespace

void dial_history_note(int64_t at_ms, uint32_t hz)
{
    if (s_count > 0 && nth(s_count - 1).hz == hz) {
        return;
    }
    if (s_count == DIAL_HISTORY_LEN) {
        s_head = (s_head + 1) % DIAL_HISTORY_LEN;
        --s_count;
    }
    s_tunes[(s_head + s_count) % DIAL_HISTORY_LEN] = Tune{at_ms, hz};
    ++s_count;
}

uint32_t dial_history_at(int64_t at_ms)
{
    for (int i = s_count - 1; i >= 0; --i) {
        if (nth(i).at_ms <= at_ms) {
            return nth(i).hz;
        }
    }
    return 0;
}

void dial_history_reset(void)
{
    s_head = 0;
    s_count = 0;
}
