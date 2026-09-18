#include "callsign_hashtable.h"

#include <cstring>

extern "C" {
#include "ft8/debug.h"
}

// 128 entries × 16 bytes = 2 KB of BSS. 256 was the original size but
// well over typical working set (FT8 rarely sees >50 unique hashed
// callsigns in an active period; the aging + eviction logic keeps it
// fresh). Reducing by 2 KB gives the USB DMA buffer (4608 bytes) a
// Keep this compact so USB host setup retains contiguous heap.
// fragmentation.
#define CALLSIGN_HASHTABLE_SIZE 128

static struct
{
    char callsign[12]; /// Up to 11 symbols of callsign + trailing zero
    uint32_t hash;     /// 8 MSBs = age, 22 LSBs = hash value
} callsign_hashtable[CALLSIGN_HASHTABLE_SIZE];

static int callsign_hashtable_size;

void hashtable_init(void)
{
    callsign_hashtable_size = 0;
    memset(callsign_hashtable, 0, sizeof(callsign_hashtable));
}

// Increment age for all existing entries (saturate at 255). Call once per slot.
void hashtable_age_all(void)
{
    for (int i = 0; i < CALLSIGN_HASHTABLE_SIZE; ++i)
    {
        if (callsign_hashtable[i].callsign[0] != '\0')
        {
            uint8_t age = (uint8_t)(callsign_hashtable[i].hash >> 24);
            if (age < 255)
            {
                age++;
                callsign_hashtable[i].hash =
                    ((uint32_t)age << 24) | (callsign_hashtable[i].hash & 0x003FFFFFu);
            }
        }
    }
}

// Trim the hash table if it grows too large by evicting the oldest entries
void hashtable_trim_size(int max_size)
{
    while (callsign_hashtable_size > max_size)
    {
        int oldest_idx = -1;
        uint8_t oldest_age = 0;

        for (int i = 0; i < CALLSIGN_HASHTABLE_SIZE; ++i)
        {
            if (callsign_hashtable[i].callsign[0] == '\0')
                continue;

            uint8_t age = (uint8_t)(callsign_hashtable[i].hash >> 24);
            if (oldest_idx < 0 || age > oldest_age)
            {
                oldest_idx = i;
                oldest_age = age;
            }
        }

        if (oldest_idx < 0)
            break;

        LOG(LOG_INFO, "Hashtable trim: removing oldest [%s], age=%u\n",
            callsign_hashtable[oldest_idx].callsign, (unsigned)oldest_age);

        callsign_hashtable[oldest_idx].callsign[0] = '\0';
        callsign_hashtable[oldest_idx].hash = 0;
        callsign_hashtable_size--;
    }
}

void hashtable_add(const char* callsign, uint32_t hash)
{
    if (!callsign || !callsign[0])
        return;

    uint32_t hash_payload = hash & 0x003FFFFFu;   // 22-bit value
    uint16_t hash10 = (hash_payload >> 12) & 0x03FFu;
    int idx = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    int start_idx = idx;

    while (callsign_hashtable_size >= CALLSIGN_HASHTABLE_SIZE)
    {
        hashtable_trim_size(CALLSIGN_HASHTABLE_SIZE - 50);
        if (callsign_hashtable_size >= CALLSIGN_HASHTABLE_SIZE)
        {
            LOG(LOG_INFO, "Hash table full; ignoring new callsign [%s]\n", callsign);
            return;
        }
    }

    // Linear probing: must match lookup logic
    while (callsign_hashtable[idx].callsign[0] != '\0')
    {
        uint32_t existing_hash = callsign_hashtable[idx].hash & 0x003FFFFFu;

        if ((existing_hash == hash_payload) &&
            (strcmp(callsign_hashtable[idx].callsign, callsign) == 0))
        {
            // Refresh age to 0, keep same callsign/hash
            callsign_hashtable[idx].hash = hash_payload;
            LOG(LOG_DEBUG, "Found duplicate [%s], refreshed age\n", callsign);
            return;
        }

        if (existing_hash == hash_payload)
        {
            // Same 22-bit hash but different callsign: replace old one
            LOG(LOG_INFO, "Replacing [%s] with [%s] on same hash\n",
                callsign_hashtable[idx].callsign, callsign);

            strncpy(callsign_hashtable[idx].callsign, callsign, 11);
            callsign_hashtable[idx].callsign[11] = '\0';
            callsign_hashtable[idx].hash = hash_payload;
            return;
        }

        idx = (idx + 1) % CALLSIGN_HASHTABLE_SIZE;
        if (idx == start_idx)
        {
            LOG(LOG_INFO, "Hash table probe wrapped; abort insert for [%s]\n", callsign);
            return;
        }
    }

    strncpy(callsign_hashtable[idx].callsign, callsign, 11);
    callsign_hashtable[idx].callsign[11] = '\0';
    callsign_hashtable[idx].hash = hash_payload;  // age=0
    callsign_hashtable_size++;
}

bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char* callsign)
{
    if (!callsign)
        return false;

    uint8_t hash_shift =
        (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 :
        (hash_type == FTX_CALLSIGN_HASH_12_BITS) ? 10 : 0;

    // Derive the same start bucket from the top 10 bits of the 22-bit hash.
    // For 10-bit lookup: hash is already the top 10 bits.
    // For 12-bit lookup: top 10 bits are hash >> 2.
    // For 22-bit lookup: top 10 bits are hash >> 12.
    uint16_t hash10 =
        (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? (hash & 0x03FFu) :
        (hash_type == FTX_CALLSIGN_HASH_12_BITS) ? ((hash >> 2) & 0x03FFu) :
                                                   ((hash >> 12) & 0x03FFu);

    int idx = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    // Important: entries can be deleted by hashtable_trim_size(), which creates
    // empty holes in probe chains. Stopping at the first empty slot can miss
    // valid entries that were inserted later in that chain. Scan the full table.
    for (int probe = 0; probe < CALLSIGN_HASHTABLE_SIZE; ++probe)
    {
        int scan_idx = (idx + probe) % CALLSIGN_HASHTABLE_SIZE;
        if (callsign_hashtable[scan_idx].callsign[0] == '\0')
            continue;

        uint32_t existing_hash = callsign_hashtable[scan_idx].hash & 0x003FFFFFu;

        if ((existing_hash >> hash_shift) == hash)
        {
            strcpy(callsign, callsign_hashtable[scan_idx].callsign);

            // Reset age to 0 on successful hit, preserve 22-bit payload.
            callsign_hashtable[scan_idx].hash = existing_hash;
            return true;
        }
    }

    callsign[0] = '\0';
    return false;
}

ftx_callsign_hash_interface_t hash_if = {
    .lookup_hash = hashtable_lookup,
    .save_hash = hashtable_add
};
