#include "dial_history.h"

#include <mutex>

namespace {

struct Tune {
    int64_t at_ms;
    uint32_t hz;
};

// A ring, oldest first from s_head; noted from the CAT path, read from the
// decode task, hence the lock.
Tune s_tunes[DIAL_HISTORY_LEN];
int s_head = 0;
int s_count = 0;
std::mutex s_lock;

const Tune& nth(int i) { return s_tunes[(s_head + i) % DIAL_HISTORY_LEN]; }

}  // namespace

void dial_history_note(int64_t at_ms, uint32_t hz)
{
    std::lock_guard<std::mutex> guard(s_lock);
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
    std::lock_guard<std::mutex> guard(s_lock);
    for (int i = s_count - 1; i >= 0; --i) {
        if (nth(i).at_ms <= at_ms) {
            return nth(i).hz;
        }
    }
    return 0;
}

void dial_history_reset(void)
{
    std::lock_guard<std::mutex> guard(s_lock);
    s_head = 0;
    s_count = 0;
}
