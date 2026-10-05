#include <math.h>

#include "decode_sync.h"

#define DECODE_SYNC_MAX_TIMES 64

int decode_sync_step_ms(const float* times_s, int n, int capture_clean)
{
    if (!capture_clean || n <= 3) return 0;
    if (n > DECODE_SYNC_MAX_TIMES) n = DECODE_SYNC_MAX_TIMES;
    float sorted[DECODE_SYNC_MAX_TIMES];
    for (int i = 0; i < n; ++i) {
        float key = times_s[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }
    float median = sorted[n / 2];
    if (fabsf(median) <= 0.3f) return 0;
    int delta_ms = (int)lrintf(-median * 1000.0f);
    if (delta_ms > 320) delta_ms = 320;
    if (delta_ms < -320) delta_ms = -320;
    return delta_ms;
}
