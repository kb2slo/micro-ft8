#pragma once

// How many channels the P4 receives and decodes at once (docs/roadmap/several-channels.md). Each one has its
// own static capture buffers and decode state, in PSRAM (docs/architecture/memory.md), so raising this costs
// RAM at build time, never at runtime; row an8x raises it to 2.
#define RX_MAX_CHANNELS 1
