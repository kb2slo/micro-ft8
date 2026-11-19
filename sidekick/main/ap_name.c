#include "ap_name.h"

#include <stdio.h>

void ap_name_default(const uint8_t ap_mac[6], char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    const int n = snprintf(out, out_len, "micro-ft8.local %02X%02X", ap_mac[4], ap_mac[5]);
    if (n != AP_NAME_DEFAULT_LEN || (size_t)n >= out_len) {
        out[0] = '\0';
    }
}
