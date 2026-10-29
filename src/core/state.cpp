#include "core/state.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <xmmintrin.h>

namespace sfv {

namespace {

constexpr uint64_t P1 = 11400714785074694791ULL;
constexpr uint64_t P2 = 14029467366897019727ULL;
constexpr uint64_t P3 = 1609587929392839161ULL;
constexpr uint64_t P4 = 9650029242287828579ULL;
constexpr uint64_t P5 = 2870177450012600261ULL;

inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t Read64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t Read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

inline uint64_t Round(uint64_t acc, uint64_t input)
{
    acc += input * P2;
    acc = Rotl(acc, 31);
    return acc * P1;
}

inline uint64_t MergeRound(uint64_t acc, uint64_t val)
{
    acc ^= Round(0, val);
    return acc * P1 + P4;
}

std::array<uint32_t, 256> MakeCrcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}

} // namespace

uint64_t Hash64(const void* data, size_t len, uint64_t seed)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    uint64_t h;

    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 32;
        do {
            v1 = Round(v1, Read64(p));
            v2 = Round(v2, Read64(p + 8));
            v3 = Round(v3, Read64(p + 16));
            v4 = Round(v4, Read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = MergeRound(h, v1);
        h = MergeRound(h, v2);
        h = MergeRound(h, v3);
        h = MergeRound(h, v4);
    }
    else {
        h = seed + P5;
    }

    h += static_cast<uint64_t>(len);

    while (p + 8 <= end) {
        h ^= Round(0, Read64(p));
        h = Rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(Read32(p)) * P1;
        h = Rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p) * P5;
        h = Rotl(h, 11) * P1;
        p++;
    }

    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

uint32_t Crc32(const void* data, size_t len, uint32_t crc)
{
    static const auto table = MakeCrcTable();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

size_t StateSize(const std::vector<StateRegion>& regions)
{
    size_t total = 0;
    for (const auto& r : regions) total += r.size;
    return total;
}

void SaveRegions(const std::vector<StateRegion>& regions, uint8_t* dst)
{
    for (const auto& r : regions) {
        std::memcpy(dst, r.ptr, r.size);
        dst += r.size;
    }
}

void LoadRegions(const std::vector<StateRegion>& regions, const uint8_t* src)
{
    for (const auto& r : regions) {
        std::memcpy(r.ptr, src, r.size);
        src += r.size;
    }
}

namespace {

AddressTest g_addressTest = nullptr;

// Hash64 of `len` bytes with every byte of a masked 8-byte word read as zero:
// the same value as Hash64 over a copy with those words cleared, without the
// copy (which doubled the cost over SFV's 436 KB of mostly masked regions).
uint64_t Hash64Masked(const uint8_t* data, size_t len, const std::vector<uint8_t>& ignore)
{
    auto word = [&](size_t offset) { return IsIgnored(ignore, offset) ? 0 : Read64(data + offset); };
    size_t at = 0;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
        do {
            // A stripe is 4 words, half a mask byte; most have no bit set.
            const size_t byte = at / 64;
            const unsigned bits = byte < ignore.size() ? (ignore[byte] >> ((at / 32) % 2 * 4)) & 0xF : 0;
            if (bits == 0) {
                v1 = Round(v1, Read64(data + at));
                v2 = Round(v2, Read64(data + at + 8));
                v3 = Round(v3, Read64(data + at + 16));
                v4 = Round(v4, Read64(data + at + 24));
            }
            else {
                v1 = Round(v1, bits & 1 ? 0 : Read64(data + at));
                v2 = Round(v2, bits & 2 ? 0 : Read64(data + at + 8));
                v3 = Round(v3, bits & 4 ? 0 : Read64(data + at + 16));
                v4 = Round(v4, bits & 8 ? 0 : Read64(data + at + 24));
            }
            at += 32;
        } while (at + 32 <= len);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = MergeRound(h, v1);
        h = MergeRound(h, v2);
        h = MergeRound(h, v3);
        h = MergeRound(h, v4);
    }
    else {
        h = P5;
    }
    h += static_cast<uint64_t>(len);
    for (; at + 8 <= len; at += 8) {
        h ^= Round(0, word(at));
        h = Rotl(h, 27) * P1 + P4;
    }
    // A partial last word: masked bytes read as zero too.
    const bool tailMasked = at < len && IsIgnored(ignore, at);
    if (at + 4 <= len) {
        h ^= static_cast<uint64_t>(tailMasked ? 0 : Read32(data + at)) * P1;
        h = Rotl(h, 23) * P2 + P3;
        at += 4;
    }
    for (; at < len; at++) {
        h ^= (tailMasked ? 0 : data[at]) * P5;
        h = Rotl(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

uint64_t HashRegion(const uint8_t* data, const StateRegion& region)
{
    const AddressTest isAddress = g_addressTest;
    if (!isAddress) return region.ignore.empty() ? Hash64(data, region.size) : Hash64Masked(data, region.size, region.ignore);

    thread_local std::vector<uint8_t> scratch;
    scratch.assign(data, data + region.size);
    for (size_t word = 0; word * 8 < region.size; word++) {
        const size_t len = std::min<size_t>(8, region.size - word * 8);
        bool masked = IsIgnored(region.ignore, word * 8);
        if (!masked && len == 8) masked = isAddress(Read64(&scratch[word * 8]));
        if (masked) std::memset(&scratch[word * 8], 0, len);
    }
    return Hash64(scratch.data(), scratch.size());
}

} // namespace

void SetAddressTest(AddressTest test) { g_addressTest = test; }

bool IsProcessAddress(uint64_t value) { return g_addressTest && g_addressTest(value); }

void HashRegions(const std::vector<StateRegion>& regions, uint64_t* out)
{
    // The regions lie all over the heap, mostly out of cache: the start of
    // the next ones is fetched while this one hashes.
    constexpr size_t kAhead = 2, kLines = 4;
    for (size_t i = 0; i < regions.size(); i++) {
        if (i + kAhead < regions.size()) {
            const StateRegion& next = regions[i + kAhead];
            for (size_t l = 0; l < kLines && l * 64 < next.size; l++) {
                _mm_prefetch(reinterpret_cast<const char*>(next.ptr) + l * 64, _MM_HINT_T0);
            }
        }
        out[i] = HashRegion(regions[i].ptr, regions[i]);
    }
}

void HashSavedState(const std::vector<StateRegion>& regions, const uint8_t* state, uint64_t* out)
{
    for (size_t i = 0; i < regions.size(); i++) {
        out[i] = HashRegion(state, regions[i]);
        state += regions[i].size;
    }
}

uint32_t FoldChecksum(const uint64_t* regionHashes, size_t count)
{
    // Order-independent: peers may list the same regions in a different order.
    thread_local std::vector<uint64_t> sorted;
    sorted.assign(regionHashes, regionHashes + count);
    std::sort(sorted.begin(), sorted.end());
    const uint64_t h = Hash64(sorted.data(), count * sizeof(uint64_t));
    return static_cast<uint32_t>(h ^ (h >> 32));
}

} // namespace sfv
