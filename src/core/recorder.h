#pragma once
// Helpers shared by the live session and the verifier for filling .sfvr files.

#include "core/replay_file.h"

#include <cstdint>
#include <vector>

namespace sfv {

std::vector<RegionInfo> DescribeRegions(const std::vector<StateRegion>& regions);

// Copies of the last `depth` simulated states, keyed by frame, so a desync
// report can carry the raw state of the frame the peers disagreed on. A frame
// that is resimulated after a rollback replaces its earlier copy.
class StateHistory {
public:
    void Reset(size_t depth, size_t stateSize);
    void Store(int frame, const std::vector<StateRegion>& regions);
    const std::vector<uint8_t>* Find(int frame) const;

private:
    struct Entry {
        int frame = -1;
        std::vector<uint8_t> bytes;
    };
    std::vector<Entry> entries_;
};

} // namespace sfv
