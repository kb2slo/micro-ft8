#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One writer, one reader. capacity is a power of two. One slot stays empty
// so full and empty are different. A write that does not fit drops the rest
// and counts it.
typedef struct Ft8SampleRing {
    float *samples;
    unsigned capacity;
    unsigned mask;
    unsigned head;
    unsigned tail;
    unsigned dropped;
} Ft8SampleRing;

// storage must hold `capacity` floats and outlive the ring.
void ft8_sample_ring_init(Ft8SampleRing *ring, float *storage, unsigned capacity);

unsigned ft8_sample_ring_write(Ft8SampleRing *ring, const float *src, unsigned n);
unsigned ft8_sample_ring_read(Ft8SampleRing *ring, float *dst, unsigned max_n);
unsigned ft8_sample_ring_count(const Ft8SampleRing *ring);
unsigned ft8_sample_ring_dropped(const Ft8SampleRing *ring);

#ifdef __cplusplus
}
#endif
