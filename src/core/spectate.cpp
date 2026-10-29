#include "core/spectate.h"

#include "core/recorder.h"
#include "core/state.h"

#include <ctime>

namespace sfv {

SpectatorPlayback::SpectatorPlayback(IGame& game, SpectateConfig config) : game_(game), config_(std::move(config)) {}

void SpectatorPlayback::Log(const std::string& msg) const
{
    if (config_.log) config_.log(msg);
}

bool SpectatorPlayback::Start(std::string* error)
{
    if (!game_.BeginMatch(config_.setup)) {
        if (error) *error = "the game cannot start the match setup";
        return false;
    }
    const auto& regions = game_.Regions();
    if (regions.empty()) {
        if (error) *error = "the game reports no rollback state";
        return false;
    }
    recording_ = ReplayFile{};
    recording_.modVersion = config_.modVersion;
    recording_.gameBuild = game_.BuildId();
    recording_.matchId = config_.matchId;
    recording_.localPlayer = -1;
    recording_.createdUnix = static_cast<uint64_t>(std::time(nullptr));
    recording_.setup = config_.setup;
    recording_.regions = DescribeRegions(regions);
    recording_.initialHashes.resize(regions.size());
    HashRegions(regions, recording_.initialHashes.data());
    hashes_.assign(regions.size(), 0);
    Log("spectating match, delay " + std::to_string(config_.delayFrames) + " frames");
    return true;
}

void SpectatorPlayback::Add(int first, const std::vector<FrameInputs>& inputs, const std::vector<uint32_t>& checksums)
{
    for (size_t i = 0; i < inputs.size(); i++) {
        const int frame = first + static_cast<int>(i);
        if (frame < received_) continue;
        pending_[frame] = {inputs[i], i < checksums.size() ? checksums[i] : 0};
    }
    for (auto it = pending_.find(received_); it != pending_.end(); it = pending_.find(received_)) {
        inputs_.push_back(it->second.first);
        checksums_.push_back(it->second.second);
        pending_.erase(it);
        received_++;
    }
}

void SpectatorPlayback::Play()
{
    const int frame = next_;
    game_.Advance(inputs_[frame], false);
    const auto& regions = game_.Regions();
    HashRegions(regions, hashes_.data());
    recording_.SetFrame(frame, inputs_[frame], hashes_.data());
    const uint32_t expected = checksums_[frame];
    bool differs = false;
    if (expected != 0 && !game_.OutcomeDecided()) {
        const uint32_t mine = FoldChecksum(hashes_.data(), hashes_.size());
        differs = mine != expected;
        if (differs && desyncCount_++ == 0) {
            desyncFrame_ = frame;
            recording_.events.push_back({frame, ReplayEventType::Desync, mine, expected});
            Log("spectator desync at frame " + std::to_string(frame));
        }
    }
    // Runs of differing frames (the first few), for the log.
    if (differs && runStart_ < 0) {
        runStart_ = frame;
    }
    else if (!differs && runStart_ >= 0) {
        if (runsLogged_++ < 10) {
            Log("checksums differed in frames " + std::to_string(runStart_) + "-" + std::to_string(frame - 1));
        }
        runStart_ = -1;
    }
    next_++;
}

void SpectatorPlayback::Update()
{
    const int buffered = received_ - next_;
    if (!started_) {
        if (buffered < config_.delayFrames) return;
        started_ = true;
    }
    if (buffered <= 0) {
        // Ran dry: wait until the delay has filled again, one longer pause
        // rather than a stutter of single frames.
        stalls_++;
        started_ = false;
        return;
    }
    Play();
}

ReplayFile SpectatorPlayback::Recording() const
{
    ReplayFile copy = recording_;
    copy.finalThrough = copy.FrameCount() - 1;
    return copy;
}

} // namespace sfv
