#include <cstdlib>
#include <cstring>

#include "heard_grid.h"

namespace {

// FT8's longest standard or nonstandard call is 11 characters.
constexpr size_t kCallMax = 11;

struct HeardGrid {
    char call[kCallMax + 1];
    char grid[5];
    int64_t heard_ms;
    bool used;
};

// Allocated on first use, not static: 5 KB of the internal RAM the decode pipeline's waterfall
// needs in one block, and the first note comes after the waterfall is allocated. Null when
// there is no RAM for it: nothing is noted and nothing is found.
HeardGrid* s_table_ptr = nullptr;

HeardGrid* table()
{
    if (!s_table_ptr) {
        s_table_ptr = static_cast<HeardGrid*>(calloc(HEARD_GRID_CAPACITY, sizeof(HeardGrid)));
    }
    return s_table_ptr;
}

// Upper case with <> stripped, as autoseq keys its entries. False for
// anything that cannot be a callsign, including the unresolved hash "<...>".
bool normalize_call(const char* in, char* out)
{
    size_t begin = 0;
    size_t end = strlen(in);
    if (end > begin && in[begin] == '<') ++begin;
    if (end > begin && in[end - 1] == '>') --end;
    const size_t len = end - begin;
    if (len < 3 || len > kCallMax) return false;
    bool has_digit = false;
    for (size_t i = 0; i < len; ++i) {
        char c = in[begin + i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c >= '0' && c <= '9') {
            has_digit = true;
        } else if (!((c >= 'A' && c <= 'Z') || c == '/')) {
            return false;
        }
        out[i] = c;
    }
    out[len] = '\0';
    return has_digit;
}

// A four-character Maidenhead square (fields A-R). RR73 has the shape but is
// the closing message.
bool normalize_grid(const char* in, char* out)
{
    if (strlen(in) != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const bool ok = (i < 2) ? (c >= 'A' && c <= 'R') : (c >= '0' && c <= '9');
        if (!ok) return false;
        out[i] = c;
    }
    out[4] = '\0';
    return strcmp(out, "RR73") != 0;
}

int find(const char* call)
{
    const HeardGrid* s_table = table();
    if (!s_table) return -1;
    for (int i = 0; i < HEARD_GRID_CAPACITY; ++i) {
        if (s_table[i].used && strcmp(s_table[i].call, call) == 0) return i;
    }
    return -1;
}

// An unused slot, or else the one heard longest ago.
int slot_for_new()
{
    const HeardGrid* s_table = table();
    int oldest = 0;
    for (int i = 0; i < HEARD_GRID_CAPACITY; ++i) {
        if (!s_table[i].used) return i;
        if (s_table[i].heard_ms < s_table[oldest].heard_ms) oldest = i;
    }
    return oldest;
}

}  // namespace

void heard_grid_clear()
{
    if (s_table_ptr) memset(s_table_ptr, 0, HEARD_GRID_CAPACITY * sizeof(HeardGrid));
}

void heard_grid_note(const char* sender, const char* field3, bool standard_message, int64_t now_ms)
{
    char call[kCallMax + 1];
    char grid[5];
    if (!standard_message || !normalize_grid(field3, grid) || !normalize_call(sender, call)) return;
    HeardGrid* s_table = table();
    if (!s_table) return;
    int idx = find(call);
    if (idx < 0) {
        idx = slot_for_new();
        memcpy(s_table[idx].call, call, sizeof(call));
        s_table[idx].used = true;
    }
    memcpy(s_table[idx].grid, grid, sizeof(grid));
    s_table[idx].heard_ms = now_ms;
}

bool heard_grid_lookup(const char* call, int64_t now_ms, std::string* grid_out)
{
    char key[kCallMax + 1];
    if (!normalize_call(call, key)) return false;
    const int idx = find(key);
    if (idx < 0 || now_ms - s_table_ptr[idx].heard_ms >= HEARD_GRID_EXPIRY_MS) return false;
    if (grid_out) *grid_out = s_table_ptr[idx].grid;
    return true;
}
