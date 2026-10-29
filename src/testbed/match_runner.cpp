#include "testbed/match_runner.h"

#include "core/state.h"

#include <memory>

namespace sfv {

PackedInput MasherInput(uint64_t seed, int poll, int player)
{
    const uint64_t key[3] = {seed, static_cast<uint64_t>(player), static_cast<uint64_t>(poll / 4)};
    const uint64_t h = Hash64(key, sizeof(key));

    PackedInput in = 0;
    switch (h % 8) {
    case 0: case 1: in |= kRight; break;
    case 2: case 3: in |= kLeft; break;
    case 4: in |= kDown; break;
    case 5: in |= kUp; break;
    default: break;
    }
    if ((h >> 8) % 3 == 0) in |= static_cast<PackedInput>(kLP << ((h >> 16) % 6));
    return in;
}

MockMatchResult RunMockMatch(const MockMatchOptions& options)
{
    MockMatchResult result;
    LoopbackNetwork net(options.seed, options.link);
    const uint64_t ids[kNumPlayers] = {0x1100001000000001ull, 0x1100001000000002ull};

    std::unique_ptr<MockGame> games[kNumPlayers];
    std::unique_ptr<NetplaySession> sessions[kNumPlayers];

    for (int p = 0; p < kNumPlayers; p++) {
        MockGameOptions go;
        go.bug = options.bug[p];
        go.machineValue = options.machineValue[p];
        go.input = [seed = options.seed](int poll, int player) { return MasherInput(seed, poll, player); };
        games[p] = std::make_unique<MockGame>(go);

        NetplayConfig nc;
        nc.localPlayer = p;
        nc.remotePeer = ids[1 - p];
        nc.matchId = options.seed * 7919u + 1;
        nc.setup = options.setup[p];
        nc.modVersion = "test";
        nc.inputDelay = options.inputDelay;
        nc.predictionWindow = options.predictionWindow;
        if (options.log) nc.log = [p, log = options.log](const std::string& m) { log("P" + std::to_string(p + 1) + ": " + m); };
        sessions[p] = std::make_unique<NetplaySession>(*games[p], net.Endpoint(ids[p]), nc);
        sessions[p]->onDesync = [&result, p](const DesyncInfo&, const ReplayFile&) { result.reportsRaised[p]++; };
        if (!sessions[p]->Start(nullptr)) return result;
    }
    result.started = true;

    for (int frame = 0; frame < options.renderedFrames; frame++) {
        for (auto& s : sessions) s->Update();
        net.Tick();
    }

    for (int p = 0; p < kNumPlayers; p++) {
        result.recording[p] = sessions[p]->Recording();
        result.desyncs[p] = sessions[p]->Desyncs();
        result.desyncCount[p] = sessions[p]->DesyncCount();
        result.rollbacks[p] = sessions[p]->RollbackStats();
    }
    return result;
}

} // namespace sfv
