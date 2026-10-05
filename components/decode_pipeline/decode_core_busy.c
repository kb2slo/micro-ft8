#include "decode_core_busy.h"

int decode_core_busy_pct(uint32_t idle_delta, uint32_t elapsed)
{
    if (elapsed == 0 || idle_delta >= elapsed) return 0;
    return (int)(((uint64_t)(elapsed - idle_delta) * 100) / elapsed);
}
