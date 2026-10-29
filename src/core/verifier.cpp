#include "core/verifier.h"

#include "core/recorder.h"
#include "core/state.h"

#include <algorithm>

namespace sfv {

namespace {

std::vector<RegionMismatch> Compare(const uint64_t* expected, const uint64_t* actual, size_t count)
{
    std::vector<RegionMismatch> out;
    for (size_t i = 0; i < count; i++) {
        if (expected[i] != actual[i]) out.push_back({i, expected[i], actual[i]});
    }
    return out;
}

std::vector<uint8_t> Capture(const std::vector<StateRegion>& regions)
{
    std::vector<uint8_t> bytes(StateSize(regions));
    SaveRegions(regions, bytes.data());
    return bytes;
}

} // namespace

const char* ToString(VerifyResult::Status status)
{
    switch (status) {
    case VerifyResult::Status::Match: return "match";
    case VerifyResult::Status::SetupMismatch: return "setup mismatch";
    case VerifyResult::Status::Diverged: return "diverged";
    case VerifyResult::Status::StressMismatch: return "rollback stress mismatch";
    case VerifyResult::Status::Unsupported: return "unsupported";
    }
    return "?";
}

VerifyResult VerifyReplay(IGame& game, const ReplayFile& replay, const VerifyOptions& options)
{
    VerifyResult result;
    auto unsupported = [&](std::string msg) {
        result.status = VerifyResult::Status::Unsupported;
        result.message = std::move(msg);
        return result;
    };

    if (game.BuildId() != replay.gameBuild) {
        return unsupported("recorded on build '" + replay.gameBuild + "', this is '" + game.BuildId() + "'");
    }
    if (!game.BeginMatch(replay.setup)) return unsupported("the game cannot start the recorded match setup");

    const std::vector<StateRegion>& regions = game.Regions();
    if (DescribeRegions(regions) != replay.regions) {
        return unsupported("the state region layout differs from the recording");
    }

    const size_t regionCount = regions.size();
    std::vector<uint64_t> hashes(regionCount);

    HashRegions(regions, hashes.data());
    result.regions = Compare(replay.initialHashes.data(), hashes.data(), regionCount);
    if (!result.regions.empty()) {
        result.status = VerifyResult::Status::SetupMismatch;
        result.frame = -1;
        result.message = "state before frame 0 differs from the recording";
        result.actualState = Capture(regions);
        return result;
    }

    int last = replay.FrameCount() - 1;
    if (options.confirmedOnly) last = std::min(last, replay.finalThrough);
    if (options.lastFrame >= 0) last = std::min(last, options.lastFrame);

    const int distance = std::max(0, options.stressDistance);
    const int every = std::max(1, options.stressEvery);
    // states[f % ring] holds the state after frame f, for stress loads.
    const size_t ring = size_t(distance) + 1;
    std::vector<std::vector<uint8_t>> states(distance > 0 ? ring : 0);
    std::vector<int> stateFrames(states.size(), -2);
    const size_t stateSize = StateSize(regions);
    for (auto& s : states) s.resize(stateSize);

    std::vector<uint64_t> resimHashes(regionCount);

    for (int f = 0; f <= last; f++) {
        game.Advance(replay.inputs[f], false);
        HashRegions(regions, hashes.data());
        result.framesChecked = f + 1;
        if (options.onFrame) options.onFrame(f);

        // Frames without region hashes (compact recordings) compare checksums.
        if (replay.HasRegionHashes(f)) result.regions = Compare(replay.FrameHashes(f), hashes.data(), regionCount);
        else result.regions.clear();
        const bool diverged = !result.regions.empty() ||
                              (!replay.HasRegionHashes(f) && FoldChecksum(hashes.data(), regionCount) != replay.FrameChecksum(f));
        if (diverged) {
            result.status = VerifyResult::Status::Diverged;
            result.frame = f;
            result.message = "simulation with the recorded inputs does not reproduce the recorded state";
            result.actualState = Capture(regions);
            return result;
        }

        if (distance == 0) continue;

        const size_t slot = size_t(f) % ring;
        SaveRegions(regions, states[slot].data());
        stateFrames[slot] = f;

        const int from = f - distance;
        if (from < 0 || f % every != 0) continue;

        const size_t fromSlot = size_t(from) % ring;
        if (stateFrames[fromSlot] != from) continue;

        LoadRegions(regions, states[fromSlot].data());
        game.OnStateLoaded(from);
        for (int g = from + 1; g <= f; g++) game.Advance(replay.inputs[g], g < f);
        HashRegions(regions, resimHashes.data());

        result.regions = Compare(hashes.data(), resimHashes.data(), regionCount);
        if (!result.regions.empty()) {
            result.status = VerifyResult::Status::StressMismatch;
            result.frame = f;
            result.message = "loading frame " + std::to_string(from) + " and resimulating " +
                             std::to_string(distance) + " frames gave a different state";
            result.actualState = Capture(regions);
            result.referenceState = states[slot];
            return result;
        }
    }

    result.status = VerifyResult::Status::Match;
    result.frame = last;
    result.message = "reproduced " + std::to_string(last + 1) + " frames";
    return result;
}

} // namespace sfv
