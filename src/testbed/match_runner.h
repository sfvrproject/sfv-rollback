#pragma once
// Runs a full netplay match between two MockGames in one process.

#include "core/netplay.h"
#include "testbed/loopback.h"
#include "testbed/mock_game.h"

namespace sfv {

// Human-ish input: random directions and buttons held for a few frames.
PackedInput MasherInput(uint64_t seed, int poll, int player);

struct MockMatchOptions {
    int renderedFrames = 3000;
    uint64_t seed = 1;
    LinkConditions link{4, 2, 0.05};
    unsigned char inputDelay = 2;
    unsigned char predictionWindow = 8;
    MockBug bug[kNumPlayers] = {MockBug::None, MockBug::None};
    uint32_t machineValue[kNumPlayers] = {0, 0};
    MatchSetup setup[kNumPlayers];
    std::function<void(const std::string&)> log;
};

struct MockMatchResult {
    bool started = false;
    ReplayFile recording[kNumPlayers];
    std::vector<DesyncInfo> desyncs[kNumPlayers];
    int desyncCount[kNumPlayers] = {};
    int reportsRaised[kNumPlayers] = {};
    GekkoRollbackStats rollbacks[kNumPlayers] = {};
    NetplaySession::SaveStats saves[kNumPlayers];
};

MockMatchResult RunMockMatch(const MockMatchOptions& options);

} // namespace sfv
