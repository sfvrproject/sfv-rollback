#pragma once
// Watching a match: plays the host's confirmed inputs into a local copy of
// the battle (which started from the same frozen state), from its start to
// its end at the normal speed, one frame per update (however late the
// spectator joined: running two ticks in one rendered frame puts the battle
// camera's clock out of step), a few frames behind so the stream's hiccups
// do not show, and compares each frame's checksum with the host's.
//
// The frames arrive in batches (lobby Inputs messages) in any order and more
// than once; NextWanted() is what to ask for next.

#include "core/game.h"
#include "core/replay_file.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sfv {

struct SpectateConfig {
    uint64_t matchId = 0;
    MatchSetup setup;
    std::string modVersion;
    // Frames kept in hand before playing (and again after running dry): the
    // stream arrives in bursts.
    int delayFrames = 20;
    std::function<void(const std::string&)> log;
};

class SpectatorPlayback {
public:
    SpectatorPlayback(IGame& game, SpectateConfig config);

    bool Start(std::string* error);
    // Confirmed frames first .. first + inputs.size() - 1, with the host's
    // checksums (0: not compared).
    void Add(int first, const std::vector<FrameInputs>& inputs, const std::vector<uint32_t>& checksums);
    // Once per rendered frame: plays zero or one frame.
    void Update();

    int NextWanted() const { return received_; } // every frame before this one is in hand
    int NextFrame() const { return next_; }       // the next frame to play
    int Buffered() const { return received_ - next_; }
    int Stalls() const { return stalls_; }
    int DesyncFrame() const { return desyncFrame_; } // first frame whose checksum differed, or -1
    int DesyncCount() const { return desyncCount_; }
    ReplayFile Recording() const;

private:
    void Play();
    void Log(const std::string& msg) const;

    IGame& game_;
    SpectateConfig config_;
    std::map<int, std::pair<FrameInputs, uint32_t>> pending_; // beyond received_
    std::vector<FrameInputs> inputs_;                          // [0, received_)
    std::vector<uint32_t> checksums_;
    int received_ = 0;
    int next_ = 0;
    bool started_ = false; // the delay has filled once
    int stalls_ = 0;
    int desyncFrame_ = -1;
    int desyncCount_ = 0;
    int runStart_ = -1; // a run of frames whose checksum differs
    int runsLogged_ = 0;
    std::vector<uint64_t> hashes_;
    ReplayFile recording_;
};

} // namespace sfv
