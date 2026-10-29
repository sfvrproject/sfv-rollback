#pragma once
// Lines up the two halves of a desync report (one .sfvr per peer).

#include "core/replay_file.h"

#include <string>
#include <vector>

namespace sfv {

struct ByteRange {
    size_t region = 0;  // in the first file's order
    size_t regionB = 0; // the region of the second file it was paired with
    size_t offset = 0;  // within the region
    size_t length = 0;
    uint64_t wordA = 0; // the aligned 8-byte word holding the first differing byte
    uint64_t wordB = 0;
};

struct DiffReport {
    bool comparable = true;      // same build, match and region layout
    bool setupDiffers = false;
    bool initialStateDiffers = false;
    int firstInputMismatch = -1; // confirmed inputs disagree: netcode bug
    int firstStateMismatch = -1; // same inputs, different state: nondeterminism
    int inputsComparedThrough = -1;
    std::vector<size_t> regionsAtMismatch;
    // From the earliest frame at or after the mismatch that both files dumped.
    int dumpFrame = -1;
    std::vector<ByteRange> bytesAtMismatch;
    std::vector<std::string> notes;
};

DiffReport DiffReplays(const ReplayFile& a, const ReplayFile& b);

// Peers may list same-size regions in a different order: for each region of
// dump a, the region of dump b with the same size and the most similar
// contents (addresses on both sides count as equal).
// On by default: words that look like 64-bit addresses on both sides are not
// reported. Off shows every differing byte.
void SetDiffIgnoresAddresses(bool ignore);

std::vector<size_t> MatchRegions(const std::vector<RegionInfo>& regions, const std::vector<uint8_t>& a,
                                 const std::vector<uint8_t>& b);

// Byte ranges (whole 8-byte words) that differ between two dumps with the same
// region layout, after pairing regions with MatchRegions. Words holding an
// address on both sides are not reported.
std::vector<ByteRange> DiffStates(const std::vector<RegionInfo>& regions, const std::vector<uint8_t>& a,
                                  const std::vector<uint8_t>& b, size_t maxRanges = 4096);

std::string FormatDiff(const DiffReport& report, const ReplayFile& a);

} // namespace sfv
