// The QSO store's journal as bytes: an entry's CRC, and reading entries out of an image that may be
// damaged (include/qso_store.h has the format).

#include "qso_store_internal.h"

namespace qso_store_detail {

// CRC-32 as zlib and Ethernet compute it, four bits at a time: 64 bytes of table, not 1 KB.
uint32_t crc32_add(uint32_t crc, const uint8_t* p, size_t n)
{
    static const uint32_t kTable[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ kTable[crc & 0x0F];
        crc = (crc >> 4) ^ kTable[crc & 0x0F];
    }
    return crc;
}

uint32_t entry_crc(const uint8_t* at, size_t len)
{
    uint32_t crc = crc32_add(0xFFFFFFFFu, at, kPrevAt);
    crc = crc32_add(crc, at + kPrevAt + 4, kEntryHeaderBytes - kPrevAt - 4 + len);
    return crc ^ 0xFFFFFFFFu;
}

// The entry at `at`, if every check on it holds.
bool read_entry(const uint8_t* image, size_t end, size_t at, JournalEntry* out)
{
    if (at > end || end - at < kQsoStoreEntryOverhead) return false;
    const uint8_t* h = image + at;
    if (h[0] != kEntryMagic || (h[13] | h[14] | h[15]) != 0) return false;
    const size_t len = rd16(h + 2);
    if (len > kQsoStoreMaxFields || end - at < kQsoStoreEntryOverhead + len) return false;
    if (rd32(h + kEntryHeaderBytes + len) != entry_crc(h, len)) return false;
    out->kind = h[1];
    out->flags = h[12];
    out->id = rd32(h + 4);
    out->prev = rd32(h + kPrevAt);
    out->fields = (const char*)h + kEntryHeaderBytes;
    out->len = len;
    out->size = kQsoStoreEntryOverhead + len;
    switch (out->kind) {
        case kPut:
        case kPatch:
            return out->id != 0;
        case kDelete:
            return out->id != 0 && len == 0;
        case kMeta:
            return out->id == 0;
        default:
            return false;
    }
}

// The first readable entry at or after `from`; `end` when there is none.
size_t next_entry(const uint8_t* image, size_t end, size_t from, JournalEntry* out)
{
    for (size_t at = from; at < end; ++at) {
        if (image[at] == kEntryMagic && read_entry(image, end, at, out)) return at;
    }
    return end;
}

}  // namespace qso_store_detail
