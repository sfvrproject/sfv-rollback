#pragma once
// Hashing and copy helpers over a game's state regions.

#include "core/game.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfv {

// XXH64. Fast enough to hash several MB of battle state every frame.
uint64_t Hash64(const void* data, size_t len, uint64_t seed = 0);

// CRC-32 (IEEE), used for replay file integrity.
uint32_t Crc32(const void* data, size_t len, uint32_t crc = 0);

size_t StateSize(const std::vector<StateRegion>& regions);

// Concatenates every region into `dst` (StateSize bytes) and back.
void SaveRegions(const std::vector<StateRegion>& regions, uint8_t* dst);
void LoadRegions(const std::vector<StateRegion>& regions, const uint8_t* src);

// True when the 8-byte word holding byte `offset` is masked out.
inline bool IsIgnored(const std::vector<uint8_t>& ignore, size_t offset)
{
    const size_t word = offset / 8;
    return word / 8 < ignore.size() && (ignore[word / 8] >> (word % 8)) & 1;
}

// Values only meaningful inside this process (heap and image addresses)
// differ between machines even when the simulation agrees, so 8-byte words
// the test accepts hash as zero. The game adapter installs it; none by default.
using AddressTest = bool (*)(uint64_t value);
void SetAddressTest(AddressTest test);
bool IsProcessAddress(uint64_t value);

// One hash per region, over live memory or over a buffer laid out by SaveRegions.
// Words masked by the region's ignore bitmap, and addresses, hash as zero.
void HashRegions(const std::vector<StateRegion>& regions, uint64_t* out);
void HashSavedState(const std::vector<StateRegion>& regions, const uint8_t* state, uint64_t* out);

// The 32-bit checksum GekkoNet exchanges for desync detection, derived from the
// per-region hashes so the two always agree. Independent of region order.
uint32_t FoldChecksum(const uint64_t* regionHashes, size_t count);

} // namespace sfv
