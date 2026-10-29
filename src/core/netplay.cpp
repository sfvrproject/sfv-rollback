#include "core/netplay.h"

#include "core/gekko_bridge.h"
#include "core/state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace sfv {

NetplaySession::NetplaySession(IGame& game, ITransport& transport, NetplayConfig config)
    : game_(game), transport_(transport), config_(std::move(config))
{
}

NetplaySession::~NetplaySession()
{
    if (session_) {
        GekkoTransportScope scope(&transport_);
        gekko_destroy(&session_);
    }
}

void NetplaySession::Log(const std::string& msg) const
{
    if (config_.log) config_.log(msg);
}

bool NetplaySession::Start(std::string* error)
{
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };

    const bool stress = config_.stressCheckDistance > 0;
    if (config_.localPlayer != 0 && config_.localPlayer != 1) return fail("local player must be 0 or 1");
    if (!game_.BeginMatch(config_.setup)) return fail("the game cannot start the match setup");

    const auto& regions = game_.Regions();
    const size_t stateSize = StateSize(regions);
    if (regions.empty() || stateSize == 0) return fail("the game reports no rollback state");

    recording_ = ReplayFile{};
    recording_.modVersion = config_.modVersion;
    recording_.gameBuild = game_.BuildId();
    recording_.matchId = config_.matchId;
    recording_.localPlayer = config_.localPlayer;
    recording_.createdUnix = static_cast<uint64_t>(std::time(nullptr));
    recording_.inputDelay = config_.inputDelay;
    recording_.predictionWindow = config_.predictionWindow;
    recording_.setup = config_.setup;
    recording_.regions = DescribeRegions(regions);
    recording_.initialHashes.resize(regions.size());
    HashRegions(regions, recording_.initialHashes.data());
    // The starting state itself, so a setup that did not sync can be diffed
    // word by word.
    StateDump& initial = recording_.dumps.emplace_back();
    initial.frame = -1;
    initial.label = "initial";
    initial.bytes.resize(stateSize);
    SaveRegions(regions, initial.bytes.data());

    frameHashes_.assign(regions.size(), 0);
    hashedFrame_ = -2;
    nextDump_ = config_.dumpEvery;
    periodicDumps_ = 0;
    history_.Reset(config_.historyDepth, stateSize);

    GekkoTransportScope scope(&transport_);
    if (!gekko_create(&session_, stress ? GekkoStressSession : GekkoGameSession)) return fail("gekko_create failed");

    GekkoConfig gc{};
    gc.num_players = kNumPlayers;
    gc.max_spectators = 0;
    gc.max_input_prediction_window = config_.predictionWindow;
    gc.input_size = sizeof(PackedInput);
    gc.state_size = static_cast<unsigned int>(stateSize);
    gc.limited_saving = false;
    gc.desync_detection = true;
    gc.check_distance = config_.stressCheckDistance;
    gekko_start(session_, &gc);

    if (stress) {
        localHandle_ = gekko_add_actor(session_, GekkoLocalPlayer, nullptr);
        remoteHandle_ = gekko_add_actor(session_, GekkoLocalPlayer, nullptr);
        phase_ = NetplayPhase::Running;
        nextFrame_ = 0;
        highestFrame_ = -1;
        Log("stress session started, check distance " + std::to_string(config_.stressCheckDistance));
        return localHandle_ == 0 && remoteHandle_ == 1 ? true : fail("unexpected GekkoNet actor handles");
    }

    gekko_net_adapter_set(session_, GekkoAdapter());
    gekko_set_disconnect_timeout(session_, config_.disconnectTimeoutMs);

    // Both peers add P1 then P2 so the handles mean the same on both sides.
    remoteAddress_ = config_.remotePeer;
    GekkoNetAddress addr{&remoteAddress_, sizeof(remoteAddress_)};
    for (int p = 0; p < kNumPlayers; p++) {
        if (p == config_.localPlayer) {
            localHandle_ = gekko_add_actor(session_, GekkoLocalPlayer, nullptr);
        }
        else {
            remoteHandle_ = gekko_add_actor(session_, GekkoRemotePlayer, &addr);
        }
    }
    if (localHandle_ != config_.localPlayer || remoteHandle_ != 1 - config_.localPlayer) {
        return fail("unexpected GekkoNet actor handles");
    }
    gekko_set_local_delay(session_, localHandle_, config_.inputDelay);

    phase_ = NetplayPhase::Syncing;
    nextFrame_ = 0;
    highestFrame_ = -1;
    Log("session started, waiting for peer");
    return true;
}

void NetplaySession::Update()
{
    if (!session_ || phase_ == NetplayPhase::Ended) return;
    GekkoTransportScope scope(&transport_);

    if (config_.stressCheckDistance > 0) {
        for (int p = 0; p < kNumPlayers; p++) {
            PackedInput input = game_.ReadLocalInput(p);
            gekko_add_local_input(session_, p, &input);
        }
    }
    else {
        PackedInput input = game_.ReadLocalInput(config_.localPlayer);
        gekko_add_local_input(session_, localHandle_, &input);
    }

    int count = 0;
    GekkoGameEvent** events = gekko_update_session(session_, &count);
    for (int i = 0; i < count; i++) HandleGameEvent(events[i]);

    count = 0;
    GekkoSessionEvent** sessionEvents = gekko_session_events(session_, &count);
    for (int i = 0; i < count; i++) HandleSessionEvent(sessionEvents[i]);

    if (reportDue_ >= 0 && (highestFrame_ >= reportDue_ || phase_ == NetplayPhase::Ended)) {
        reportDue_ = -1;
        if (onDesync) onDesync(desyncs_.front(), Recording());
    }
}

void NetplaySession::Leave()
{
    if (!session_ || phase_ == NetplayPhase::Ended) return;
    GekkoTransportScope scope(&transport_);
    gekko_disconnect_actor(session_, localHandle_);
    phase_ = NetplayPhase::Ended;
}

namespace {
using CostClock = std::chrono::steady_clock;
double MicrosSince(CostClock::time_point t)
{
    return std::chrono::duration<double, std::micro>(CostClock::now() - t).count();
}
constexpr int kCostEvery = 600;
} // namespace

void NetplaySession::HandleGameEvent(GekkoGameEvent* ev)
{
    const auto& regions = game_.Regions();

    switch (ev->type) {
    case GekkoAdvanceEvent: {
        const auto& adv = ev->data.adv;
        FrameInputs in;
        std::memcpy(in.p, adv.inputs, std::min<size_t>(adv.input_len, sizeof(in.p)));

        auto t = CostClock::now();
        game_.Advance(in, adv.rolling_back);
        (adv.rolling_back ? costs_.replayTick : costs_.tick) += MicrosSince(t);
        (adv.rolling_back ? costs_.replayed : costs_.ticks)++;
        t = CostClock::now();
        HashRegions(regions, frameHashes_.data());
        costs_.hash += MicrosSince(t);
        hashedFrame_ = adv.frame;
        t = CostClock::now();

        // Stress mode resimulates frames it has simulated before with the same
        // inputs: name the regions, and the words, that came out different.
        if (config_.stressCheckDistance > 0 && adv.frame < recording_.FrameCount() &&
            std::memcmp(recording_.FrameHashes(adv.frame), frameHashes_.data(), regions.size() * 8) != 0) {
            const uint64_t* before = recording_.FrameHashes(adv.frame);
            const std::vector<uint8_t>* previous = history_.Find(adv.frame);
            std::string msg = "frame " + std::to_string(adv.frame) + " resimulated differently:";
            int listed = 0;
            size_t base = 0;
            for (size_t r = 0; r < regions.size(); base += regions[r].size, r++) {
                if (before[r] == frameHashes_[r]) continue;
                if (listed++ < 8) msg += " " + regions[r].name;
                if (!previous || !onStressDiff) continue;
                for (size_t w = 0; w < regions[r].size; w += 8) {
                    const size_t n = std::min<size_t>(8, regions[r].size - w);
                    if (std::memcmp(previous->data() + base + w, regions[r].ptr + w, n) != 0) {
                        onStressDiff(adv.frame, r, w);
                    }
                }
            }
            if (stressReports_++ < 8) Log(msg);
        }

        recording_.SetFrame(adv.frame, in, frameHashes_.data());
        if (decided_.size() <= size_t(adv.frame)) decided_.resize(adv.frame + 1, 0);
        decided_[adv.frame] = game_.OutcomeDecided();
        history_.Store(adv.frame, regions);
        costs_.record += MicrosSince(t);
        if (costs_.ticks + costs_.replayed >= kCostEvery) {
            Costs& c = costs_;
            const int n = c.ticks + c.replayed;
            // Per advance unless said; "per frame" is the whole work over the
            // frames that were not replays (about one per rendered frame).
            char buf[480];
            std::snprintf(buf, sizeof(buf),
                          "costs over %d advances (%d replayed, %d loads, %d of %d saves skipped%s): tick %.0f us, "
                          "replayed tick %.0f us, hash %.0f us, recording+history %.0f us, save %.0f us; "
                          "load %.0f us + callbacks %.0f us per load; %.2f ms per frame",
                          n, c.replayed, c.loads, c.skippedSaves, c.saves + c.skippedSaves,
                          c.loadMismatches ? (", " + std::to_string(c.loadMismatches) + " LOAD MISMATCHES").c_str() : "",
                          c.ticks ? c.tick / c.ticks : 0, c.replayed ? c.replayTick / c.replayed : 0, c.hash / n, c.record / n,
                          (c.save) / std::max(1, c.saves + c.skippedSaves), c.loads ? c.load / c.loads : 0,
                          c.loads ? c.loadCallbacks / c.loads : 0,
                          (c.tick + c.replayTick + c.hash + c.record + c.save + c.load + c.loadCallbacks) / 1000.0 /
                              std::max(1, c.ticks));
            Log(buf);
            c = {};
        }
        nextFrame_ = adv.frame + 1;
        highestFrame_ = std::max(highestFrame_, adv.frame);

        // A frame this far back has its remote input and any rollback for it
        // done: its state is final on both peers.
        if (config_.dumpEvery > 0 && periodicDumps_ < config_.maxDumps) {
            const int confirmed = highestFrame_ - int(config_.predictionWindow) - 1;
            while (nextDump_ <= confirmed && periodicDumps_ < config_.maxDumps) {
                if (const auto* state = history_.Find(nextDump_)) {
                    recording_.dumps.push_back({nextDump_, "periodic", *state});
                    periodicDumps_++;
                }
                nextDump_ += config_.dumpEvery;
            }
        }
        break;
    }
    case GekkoSaveEvent: {
        const auto& save = ev->data.save;
        const auto t = CostClock::now();
        if (save.frame != hashedFrame_) {
            HashRegions(regions, frameHashes_.data());
            hashedFrame_ = save.frame;
        }
        // GekkoNet loads frame (first mispredicted - 1) only, and confirmed
        // inputs never change: with the inputs of frame + 1 confirmed this
        // state can never be loaded (a stress session confirms nothing).
        const bool skip = config_.skipUnloadableSaves && gekko_confirmed_frame(session_) >= save.frame + 1;
        if (!skip) SaveRegions(regions, save.state);
        *save.state_len = static_cast<unsigned int>(StateSize(regions));
        const uint32_t sum = FoldChecksum(frameHashes_.data(), frameHashes_.size());
        saved_[save.frame % saved_.size()] = {save.frame, sum, skip};
        // After the outcome every state compares equal: the win screens
        // follow each machine's own menu input.
        *save.checksum = game_.OutcomeDecided() ? 0 : sum;
        costs_.save += MicrosSince(t);
        (skip ? costs_.skippedSaves : costs_.saves)++;
        saveCounts_.saves++;
        if (skip) saveCounts_.skipped++;
        break;
    }
    case GekkoLoadEvent: {
        const auto& load = ev->data.load;
        auto t = CostClock::now();
        const SavedFrame& was = saved_[load.frame % saved_.size()];
        if (was.frame == load.frame && was.skipped) {
            // Cannot happen if the rule above holds; the state loaded is stale.
            Log("load of frame " + std::to_string(load.frame) + ", whose save was skipped: the state is wrong from here");
        }
        LoadRegions(regions, load.state);
        if (config_.verifyLoads && was.frame == load.frame) {
            HashRegions(regions, frameHashes_.data());
            const uint32_t sum = FoldChecksum(frameHashes_.data(), frameHashes_.size());
            if (sum != was.checksum) {
                costs_.loadMismatches++;
                saveCounts_.loadMismatches++;
                if (loadMismatches_++ < 8) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "load of frame %d: checksum %08x, saved as %08x", load.frame, sum,
                                  was.checksum);
                    Log(buf);
                }
            }
        }
        costs_.load += MicrosSince(t);
        t = CostClock::now();
        game_.OnStateLoaded(load.frame);
        costs_.loadCallbacks += MicrosSince(t);
        costs_.loads++;
        saveCounts_.loads++;
        recording_.events.push_back({load.frame, ReplayEventType::Rollback,
                                     static_cast<uint32_t>(std::max(0, nextFrame_ - 1 - load.frame)), 0});
        hashedFrame_ = -2;
        nextFrame_ = load.frame + 1;
        break;
    }
    default:
        break;
    }
}

void NetplaySession::HandleSessionEvent(GekkoSessionEvent* ev)
{
    switch (ev->type) {
    case GekkoPlayerConnected:
        Log("peer connected");
        break;
    case GekkoSessionStarted:
        phase_ = NetplayPhase::Running;
        Log("match running");
        break;
    case GekkoPlayerDisconnected:
        Log("peer disconnected");
        recording_.events.push_back({highestFrame_, ReplayEventType::Disconnect, 0, 0});
        phase_ = NetplayPhase::Ended;
        break;
    case GekkoDesyncDetected: {
        // Once diverged, every later health check fails too. Health checks can
        // arrive out of order, so the first event is not always the earliest
        // frame: keep the dump of the earliest one among the first few events,
        // which both peers then most likely share.
        const auto& d = ev->data.desynced;
        desyncCount_++;
        if (desyncs_.size() >= kMaxRecordedDesyncs) break;

        const DesyncInfo info{d.frame, d.local_checksum, d.remote_checksum};
        desyncs_.push_back(info);
        recording_.events.push_back({d.frame, ReplayEventType::Desync, d.local_checksum, d.remote_checksum});
        if (desyncs_.size() == 1) {
            Log("desync detected at frame " + std::to_string(d.frame));
            reportDue_ = highestFrame_ + kReportDelayFrames;
        }

        StateDump* dump = nullptr;
        for (auto& existing : recording_.dumps) {
            if (existing.label == "desync") dump = &existing;
        }
        if (!dump || d.frame < dump->frame) {
            if (const auto* state = history_.Find(d.frame)) {
                if (!dump) dump = &recording_.dumps.emplace_back();
                *dump = {d.frame, "desync", *state};
            }
            else {
                Log("state of desynced frame " + std::to_string(d.frame) + " is no longer held; raise historyDepth");
            }
        }
        break;
    }
    default:
        break;
    }
}

ReplayFile NetplaySession::Recording() const
{
    ReplayFile copy = recording_;
    // GekkoNet never predicts further than the prediction window past the last
    // confirmed remote input, so anything older is final. This is the same
    // frame its desync health check compares.
    copy.finalThrough = std::min(highestFrame_ - int(config_.predictionWindow) - 1, copy.FrameCount() - 1);
    return copy;
}

int NetplaySession::ConfirmedThrough() const
{
    return std::min(highestFrame_ - int(config_.predictionWindow) - 1, recording_.FrameCount() - 1);
}

bool NetplaySession::ConfirmedFrame(int frame, FrameInputs* inputs, uint32_t* checksum) const
{
    if (frame < 0 || frame > ConfirmedThrough()) return false;
    *inputs = recording_.inputs[frame];
    *checksum = size_t(frame) < decided_.size() && decided_[frame] ? 0 : recording_.FrameChecksum(frame);
    return true;
}

void NetplaySession::SetPredictionWindow(unsigned char window)
{
    if (session_) gekko_set_input_prediction_window(session_, window);
}

void NetplaySession::SetInputDelay(unsigned char delay)
{
    if (session_) gekko_set_local_delay(session_, localHandle_, delay);
}

float NetplaySession::FramesAhead()
{
    if (!session_) return 0.0f;
    return gekko_frames_ahead(session_);
}

GekkoNetworkStats NetplaySession::NetworkStats()
{
    GekkoNetworkStats stats{};
    if (session_) gekko_network_stats(session_, remoteHandle_, &stats);
    return stats;
}

GekkoRollbackStats NetplaySession::RollbackStats()
{
    GekkoRollbackStats stats{};
    if (session_) gekko_rollback_stats(session_, &stats);
    return stats;
}

} // namespace sfv
