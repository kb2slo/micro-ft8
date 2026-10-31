#pragma once

// Mark a long-lived decode buffer for PSRAM when the P4 build enables it
// (docs/architecture/memory.md, row p3x6). Without
// CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY the attribute is empty, so the
// same source stays in internal BSS for host tools and for the internal-RAM
// decode_bench A/B image.

#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#include "esp_attr.h"
#define DECODE_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define DECODE_PSRAM_BSS
#endif
