#pragma once
// Drives one netplay match: a GekkoNet game session over an ITransport, applied
// to an IGame, recorded into an .sfvr file as it goes.

#include "core/game.h"
#include "core/recorder.h"
#include "core/replay_file.h"
#include "core/transport.h"

#include <gekkonet.h>

#include <functional>
#include <string>
#include <vector>

namespace sfv {

struct NetplayConfig {
    int localPlayer = 0; // 0 = P1, 1 = P2; the host is P1
    uint64_t remotePeer = 0;
    uint64_t matchId = 0;
    MatchSetup setup;
    std::string modVersion;
    unsigned char inputDelay = 2;
    unsigned char predictionWindow = 8;
    unsigned int disconnectTimeoutMs = 5000;
    // Simulated states kept for the state dump in desync reports. GekkoNet
    // checks frame (current - predictionWindow - 1) and learns the remote
    // checksum a round trip later, so this must cover the window plus the
    // latency in frames. Each slot costs one state copy per frame; 0 disables.
    unsigned int historyDepth = 40;
    // > 0: no peer. A GekkoNet stress session with both players local that
    // reloads and resimulates this many frames back every frame and compares
    // checksums: the sync test, through the same save/load/advance path.
    unsigned char stressCheckDistance = 0;
    // > 0: the recording also keeps the full state of every this many
    // confirmed frames (up to maxDumps), to compare the peers' states over a
    // whole match, not only at the first desync.
    int dumpEvery = 0;
    size_t maxDumps = 24;
    std::function<void(const std::string&)> log;
};

enum class NetplayPhase { Syncing, Running, Ended };

struct DesyncInfo {
    int frame = 0;
    uint32_t localChecksum = 0;
    uint32_t remoteChecksum = 0;
};

class NetplaySession {
public:
    NetplaySession(IGame& game, ITransport& transport, NetplayConfig config);
    ~NetplaySession();
    NetplaySession(const NetplaySession&) = delete;
    NetplaySession& operator=(const NetplaySession&) = delete;

    bool Start(std::string* error);
    // Once per rendered frame. Runs zero (stall), one, or several (rollback)
    // simulation ticks through the game.
    void Update();
    void Leave();

    NetplayPhase Phase() const { return phase_; }
    // The first few desync events, in arrival order, and how many there were.
    const std::vector<DesyncInfo>& Desyncs() const { return desyncs_; }
    int DesyncCount() const { return desyncCount_; }
    int NextFrame() const { return nextFrame_; }
    float FramesAhead();
    // How far past the remote's last input this side may predict, up to the
    // configured window; 0 runs in lockstep (still correcting frames already
    // predicted). Local policy: the peers need not agree.
    void SetPredictionWindow(unsigned char window);
    // The local player's input delay from now on. Raising it repeats the last
    // input; lowering it drops local inputs until the frames catch up (both
    // GekkoNet). Local policy as well: the inputs sent are the inputs.
    void SetInputDelay(unsigned char delay);
    GekkoNetworkStats NetworkStats();
    GekkoRollbackStats RollbackStats();

    // The recording so far with its confirmed range updated.
    ReplayFile Recording() const;

    // For spectators: the last frame whose inputs are confirmed (-1 before
    // any), and one such frame's inputs with its checksum (0 from the frame
    // the outcome was decided on: not compared).
    int ConfirmedThrough() const;
    bool ConfirmedFrame(int frame, FrameInputs* inputs, uint32_t* checksum) const;

    // Raised once per match, shortly after the first desync is detected (or
    // when the session ends first), with the recording at that moment. It
    // includes a dump of the earliest desynced frame's state when still held.
    std::function<void(const DesyncInfo&, const ReplayFile&)> onDesync;

    // Stress mode: each 8-byte word (by region and offset) that came out
    // different when a frame was resimulated, while the previous simulation of
    // that frame is still held (historyDepth must exceed the check distance).
    std::function<void(int frame, size_t region, size_t offset)> onStressDiff;

private:
    static constexpr size_t kMaxRecordedDesyncs = 8;
    // Lets late, reordered health checks reveal an earlier desynced frame.
    static constexpr int kReportDelayFrames = 30;

    void HandleGameEvent(GekkoGameEvent* ev);
    void HandleSessionEvent(GekkoSessionEvent* ev);
    void Log(const std::string& msg) const;

    IGame& game_;
    ITransport& transport_;
    NetplayConfig config_;
    GekkoSession* session_ = nullptr;
    int localHandle_ = -1;
    int remoteHandle_ = -1;
    uint64_t remoteAddress_ = 0;
    NetplayPhase phase_ = NetplayPhase::Syncing;
    int nextFrame_ = 0;
    int highestFrame_ = -1;
    int reportDue_ = -1;
    int stressReports_ = 0;
    int desyncCount_ = 0;
    int nextDump_ = 0;
    size_t periodicDumps_ = 0;

    ReplayFile recording_;
    StateHistory history_;
    std::vector<uint64_t> frameHashes_;
    int hashedFrame_ = -2; // frame whose hashes frameHashes_ holds
    std::vector<DesyncInfo> desyncs_;
    std::vector<uint8_t> decided_; // per frame: OutcomeDecided after it
};

} // namespace sfv
