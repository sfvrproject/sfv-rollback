#include "core/recorder.h"

#include "core/state.h"

namespace sfv {

std::vector<RegionInfo> DescribeRegions(const std::vector<StateRegion>& regions)
{
    std::vector<RegionInfo> out;
    out.reserve(regions.size());
    for (const auto& r : regions) out.push_back({r.name, r.size, r.ignore});
    return out;
}

void StateHistory::Reset(size_t depth, size_t stateSize)
{
    entries_.assign(depth, Entry{});
    for (auto& e : entries_) e.bytes.resize(stateSize);
}

void StateHistory::Store(int frame, const std::vector<StateRegion>& regions)
{
    if (entries_.empty() || frame < 0) return;
    Entry& e = entries_[size_t(frame) % entries_.size()];
    e.frame = frame;
    SaveRegions(regions, e.bytes.data());
}

const std::vector<uint8_t>* StateHistory::Find(int frame) const
{
    if (entries_.empty() || frame < 0) return nullptr;
    const Entry& e = entries_[size_t(frame) % entries_.size()];
    return e.frame == frame ? &e.bytes : nullptr;
}

} // namespace sfv
