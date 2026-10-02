#include "ft8_sample_ring.h"

void ft8_sample_ring_init(Ft8SampleRing *ring, float *storage, unsigned capacity)
{
    ring->samples = storage;
    ring->capacity = capacity;
    ring->mask = capacity - 1;
    ring->head = 0;
    ring->tail = 0;
    ring->dropped = 0;
}

unsigned ft8_sample_ring_write(Ft8SampleRing *ring, const float *src, unsigned n)
{
    const unsigned tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    const unsigned head = ring->head;
    const unsigned used = (head - tail) & ring->mask;
    const unsigned space = ring->mask - used;
    const unsigned n_store = n < space ? n : space;
    for (unsigned i = 0; i < n_store; ++i) {
        ring->samples[(head + i) & ring->mask] = src[i];
    }
    __atomic_store_n(&ring->head, (head + n_store) & ring->mask, __ATOMIC_RELEASE);
    if (n > n_store) {
        __atomic_fetch_add(&ring->dropped, n - n_store, __ATOMIC_RELAXED);
    }
    return n_store;
}

unsigned ft8_sample_ring_read(Ft8SampleRing *ring, float *dst, unsigned max_n)
{
    const unsigned head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    const unsigned tail = ring->tail;
    const unsigned used = (head - tail) & ring->mask;
    const unsigned n_take = max_n < used ? max_n : used;
    for (unsigned i = 0; i < n_take; ++i) {
        dst[i] = ring->samples[(tail + i) & ring->mask];
    }
    __atomic_store_n(&ring->tail, (tail + n_take) & ring->mask, __ATOMIC_RELEASE);
    return n_take;
}

unsigned ft8_sample_ring_count(const Ft8SampleRing *ring)
{
    const unsigned head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    const unsigned tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    return (head - tail) & ring->mask;
}

unsigned ft8_sample_ring_dropped(const Ft8SampleRing *ring)
{
    return __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED);
}
