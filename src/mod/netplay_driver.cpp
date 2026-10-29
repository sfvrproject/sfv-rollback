#include "mod/netplay_driver.h"

#include "core/match_code.h"
#include "core/netplay.h"
#include "core/replay_file.h"
#include "core/spectate.h"
#include "core/state.h"
#include "mod/battle_driver.h"
#include "mod/battle_setup.h"
#include "mod/control.h"
#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/sfv_game.h"
#include "mod/status.h"
#include "mod/sfv_offsets.h"
#include "mod/udp_transport.h"
#include "mod/weyve_service.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace sfv::mod {

namespace {

using Clock = std::chrono::steady_clock;
using lobby::Choice;
using lobby::Role;
using lobby::SessionPhase;

constexpr uint8_t kControlChannel = 1;
constexpr uint8_t kGekkoChannel = 2;
// Frames to stay frozen after the warm-up tick before reporting "loaded":
// gives asset streaming time to settle on both machines.
constexpr int kSettleFrames = 90;
// The first battle after starting the game loads its characters cold (30 s
// seen from stage select).
constexpr int kDirectJumpSeconds = 60;
constexpr int kMenuAfterMatchFrames = 60 * 3;
constexpr uint8_t kPredictionWindow = 8;
constexpr int kMaxInputDelay = 10;
// Waiting for the frozen states to be reconciled before starting anyway.
constexpr int kReconcileSeconds = 15;
// The heartbeat (PlayerState / SessionState) and the resends.
constexpr auto kBeat = std::chrono::milliseconds(250);
// Nothing heard from the other side for this long ends the session.
constexpr auto kSilence = std::chrono::seconds(10);
// The server (or the room) not answering for this long.
constexpr auto kConnectTimeout = std::chrono::seconds(15);
// Hello unanswered for this long: the host cannot be reached.
constexpr auto kJoinTimeout = std::chrono::seconds(20);
// Both ready but a game still loading characters: start anyway after this.
constexpr auto kPreloadWait = std::chrono::seconds(20);
constexpr size_t kMaxSpectators = 8;
// Spectator stream: batches in flight per spectator before an ack, and how
// long without progress before resending from the last ack.
constexpr int kStreamBatchesAhead = 4;
constexpr auto kStreamResend = std::chrono::milliseconds(300);
constexpr size_t kStreamsKept = 3;

// A value that may be an address in this process (or the peer's): never
// copied between the peers.
bool PointerLike(uint64_t v)
{
    return v >= 0x10000000000ull && v < 0x800000000000ull;
}

// Characters the random setup draws from (codes from the game's own table).
const char* const kRandomCharacters[] = {"RYU", "KEN", "CNL", "CMY", "NSH", "VEG", "KRN", "LAR", "FAN", "RSD",
                                         "RMK", "ZGF", "NCL", "DSM", "BLR", "ALX", "GUL", "IBK", "JRI", "URN"};

using State = NetView::Phase;

// What follows the current match, once the game thread has let go of it.
enum class Next { None, Jump, Lobby, End };

// How the last session ended, or the last failure (for the overlay).
std::mutex g_resultMutex;
std::string g_lastResult;

void SetResult(const std::string& text)
{
    std::lock_guard lock(g_resultMutex);
    g_lastResult = text;
}

std::string LastResult()
{
    std::lock_guard lock(g_resultMutex);
    return g_lastResult;
}

std::string Hex64(uint64_t v)
{
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

void CopyToClipboard(const std::string& text)
{
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
}

// The Steam persona name, through the game's steam_api64.dll (ISteamFriends,
// whose first method is GetPersonaName); "" when Steam is not up.
std::string SteamPersonaName()
{
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return "";
    using Getter = void* (*)();
    const auto friends = reinterpret_cast<Getter>(GetProcAddress(steam, "SteamFriends"));
    void* iface = friends ? friends() : nullptr;
    if (!iface) return "";
    using GetPersonaName = const char* (*)(void*);
    const char* name = (*reinterpret_cast<GetPersonaName**>(iface))[0](iface);
    return name ? name : "";
}

void PressPad(int pad, uint16_t buttons, int holdMs = 90, int gapMs = 600)
{
    SetScriptedPad(pad, true, buttons);
    Sleep(holdMs);
    SetScriptedPad(pad, true, 0);
    Sleep(gapMs);
}

// From a running battle to stage select through the pause menu (it always
// opens at the top; its third entry leads to stage select). False if the
// battle would not pause.
bool LeaveBattleForStageSelect(SfvGame& game, const std::atomic<bool>& stop)
{
    // Pause is ignored during round intros and KOs: retry until the battle
    // stops ticking. A battle that keeps ticking through that long is over
    // and showing its results menu, where both players have to pick
    // "Return to Character/Stage Select" (second entry).
    for (int tries = 0; !stop && !Stopping() && game.BattleTicking(); tries++) {
        if (tries == 8) {
            Log("netplay: no pause menu; leaving through the results menu");
            PressPad(0, XINPUT_GAMEPAD_DPAD_DOWN);
            PressPad(0, XINPUT_GAMEPAD_A, 90, 900);
            PressPad(1, XINPUT_GAMEPAD_DPAD_DOWN);
            PressPad(1, XINPUT_GAMEPAD_A, 90, 6000);
            SetScriptedPad(1, false);
            if (game.BattleTicking()) {
                Log("netplay: the battle does not pause");
                return false;
            }
            return true;
        }
        PressPad(0, XINPUT_GAMEPAD_START, 90, 1200);
    }
    if (stop || Stopping()) return false;
    PressPad(0, XINPUT_GAMEPAD_DPAD_DOWN);
    PressPad(0, XINPUT_GAMEPAD_DPAD_DOWN);
    PressPad(0, XINPUT_GAMEPAD_A, 90, 1500);
    PressPad(0, XINPUT_GAMEPAD_A, 90, 2500);
    return true;
}

// What differs between the battles the two sides loaded, or "".
std::string SetupMismatch(const MatchSetup& a, const MatchSetup& b)
{
    std::string out;
    auto add = [&](const std::string& what) { out += (out.empty() ? "" : ", ") + what; };
    // Not the stage: the id in the battle data is not always written by the
    // time the battle runs (battle_setup.h), so it can differ for the same
    // stage. Matching stages are up to the players for now.
    for (int p = 0; p < 2; p++) {
        const PlayerSetup &x = a.players[p], &y = b.players[p];
        const std::string who = "P" + std::to_string(p + 1) + " ";
        if (x.character != y.character) add(who + "character " + UnpackCode(x.character) + " vs " + UnpackCode(y.character));
        if (x.costume != y.costume) add(who + "costume");
        if (x.color != y.color) add(who + "color");
        if (x.vskill != y.vskill) add(who + "V-Skill");
        if (x.vtrigger != y.vtrigger) add(who + "V-Trigger");
        if (x.comment != y.comment) add(who + "comment");
    }
    return out;
}

const char* RejectText(lobby::RejectReason r)
{
    switch (r) {
    case lobby::RejectReason::WrongCode: return "wrong code";
    case lobby::RejectReason::BuildMismatch: return "different mod build";
    case lobby::RejectReason::Busy: return "the session is full";
    case lobby::RejectReason::ProtocolMismatch: return "different protocol";
    case lobby::RejectReason::NoSpectators: return "the host does not allow spectators";
    }
    return "?";
}

// The host's view of a spectator.
struct Watcher {
    std::string name;
    Clock::time_point heard;
    uint32_t heardTime = 0;            // its last heartbeat timestamp, and when
    Clock::time_point heardTimeAt;
    uint64_t match = 0;                // the match it is loading or watching
    bool loaded = false;               // ... frozen at frame 0 (its Loaded)
    uint32_t checksum = 0;
    bool reconciled = false;
    bool streaming = false;            // playing; acks frames
    int wanted = 0;                    // next frame it needs
    int sentThrough = -1;
    Clock::time_point progressAt;      // when `wanted` last moved
};

class NetplayDriver : public IFrameDriver {
public:
    NetplayDriver(SfvGame& game, std::wstring reportDir, std::string modVersion)
        : game_(game), reportDir_(std::move(reportDir)), modVersion_(std::move(modVersion)), rng_(GetTickCount64()),
          epoch_(Clock::now())
    {
    }

    ~NetplayDriver() override { Stop(); }

    bool Open(const NetplayOptions& o, bool host, Role role, std::string* error)
    {
        options_ = o;
        // Only the input delay is the players' to change; the window stays 8.
        options_.predictionWindow = kPredictionWindow;
        host_ = host;
        role_ = host ? Role::Player : role;
        name_ = o.name.empty() ? SteamPersonaName() : o.name;
        if (name_.empty()) name_ = "Player";
        if (name_.size() > lobby::kMaxName) name_.resize(lobby::kMaxName);
        link_ = static_cast<lobby::Link>(DetectLinkType());
        if (o.udp) {
            auto t = UdpTransport::Create(host ? o.udpPort : 0, error);
            if (!t) return false;
            localId_ = UdpAddress(ParseIpv4(o.udpHost), t->Port());
            base_ = std::move(t);
        }
        else {
            auto w = WeyveService::Start(o.server, name_);
            weyve_ = w.get();
            base_ = std::move(w);
        }
        conditioned_ = std::make_unique<ConditionedTransport>(*base_, rng_());
        conditioned_->SetConditions(o.conditions);
        mux_ = std::make_unique<Mux>(*conditioned_);
        control_ = &mux_->Channel(kControlChannel);
        gekko_ = &mux_->Channel(kGekkoChannel);
        build_ = Hash64(game_.BuildId().data(), game_.BuildId().size()) ^ Hash64(modVersion_.data(), modVersion_.size());
        seats_[0].present = host;
        if (host) seats_[0].name = name_;
        ready_ = o.autoReady;
        return true;
    }

    void StartHost()
    {
        connectingSince_ = Clock::now();
        if (weyve_) {
            weyve_->CreateRoom();
            SetState(State::Connecting);
            Log("netplay: hosting as %s; creating a room on %s", name_.c_str(), options_.server.c_str());
        }
        else {
            nonce_ = static_cast<uint16_t>(rng_() | 1);
            MatchCode mc;
            mc.kind = MatchCode::Kind::Udp;
            mc.address = localId_;
            mc.nonce = nonce_;
            HostReady(EncodeMatchCode(mc));
        }
        StartThread();
    }

    bool StartJoin(const std::string& code)
    {
        MatchCode mc;
        if (!DecodeMatchCode(code, &mc)) return false;
        if ((mc.kind == MatchCode::Kind::Udp) != options_.udp) {
            Log("netplay: the code is for %s; switch transport in sfv_rollback.ini", mc.kind == MatchCode::Kind::Udp ? "UDP" : "rooms");
            return false;
        }
        code_ = EncodeMatchCode(mc);
        connectingSince_ = Clock::now();
        if (weyve_) {
            weyve_->JoinRoom(mc.room);
            SetState(State::Connecting);
            Log("netplay: joining room %s on %s as %s%s", code_.c_str(), options_.server.c_str(), name_.c_str(),
                role_ == Role::Spectator ? " (spectator)" : "");
        }
        else {
            peer_ = mc.address;
            nonce_ = mc.nonce;
            BeginHello();
        }
        StartThread();
        return true;
    }

    void SetConditions(const NetConditions& c)
    {
        if (conditioned_) conditioned_->SetConditions(c);
        Log("netplay: simulating %d ms latency, %d ms jitter, %.1f%% loss", c.latencyMs, c.jitterMs, c.loss * 100.0);
    }

    void SetReady(bool ready)
    {
        ready_ = ready;
        Log("netplay: %sready", ready ? "" : "not ");
    }

    void SetPick(bool hasPick, const PlayerSetup& pick)
    {
        std::lock_guard lock(mutex_);
        if (options_.hasPick == hasPick && options_.pick == pick) return;
        options_.hasPick = hasPick;
        options_.pick = pick;
        ready_ = false; // a new pick has to be confirmed again
    }

    // Between matches (lobby, post-match): this player's input delay for the
    // next match.
    void SetDelay(uint8_t frames)
    {
        nextDelay_ = std::min<int>(frames, kMaxInputDelay);
        Log("netplay: input delay %d from the next match", nextDelay_.load());
    }

    // The host, in the lobby: the stage (or a stage setting) of the next
    // matches: a stage code, "random" or "" (as prepared).
    void SetStage(const std::string& code)
    {
        const int index = code.empty() || code == "random" ? -1 : StageIndex(code);
        hostStageRule_ = code == "random" ? lobby::kRandomStage : index >= 0 ? index : lobby::kAnyStage;
        Log("netplay: stage %s", code.empty() ? "as prepared" : code.c_str());
    }

    void Choose(Choice c)
    {
        myChoice_ = c;
        Log("netplay: after the match: %s", c == Choice::Rematch ? "rematch" : c == Choice::CharacterSelect ? "character select" : "none");
    }

    std::string Status()
    {
        std::lock_guard lock(mutex_);
        return status_;
    }

    NetView View()
    {
        std::lock_guard lock(mutex_);
        NetView v = live_;
        v.phase = state_;
        v.host = host_;
        v.spectator = role_ == Role::Spectator;
        v.udp = options_.udp;
        v.code = code_;
        v.status = status_;
        v.server = options_.udp ? "" : options_.server;
        for (int i = 0; i < kNumPlayers; i++) v.seats[i] = seats_[i];
        if (role_ == Role::Player) {
            // This side's own seat as it is now (the host's copy may lag).
            lobby::Seat& own = v.seats[host_ ? 0 : 1];
            own.ready = ready_;
            own.choice = myChoice_;
            own.hasPick = options_.hasPick;
            own.pick = options_.pick;
            own.inputDelay = nextDelay_ >= 0 ? static_cast<uint8_t>(nextDelay_.load()) : options_.inputDelay;
            own.link = link_;
        }
        v.desyncReport = desyncReport_;
        v.you = role_ == Role::Spectator ? -1 : host_ ? 0 : 1;
        v.played = played_;
        v.spectators = host_ ? static_cast<int>(watchers_.size()) : spectators_;
        v.linkPingMs = linkPingMs_;
        v.linkPath = linkPath_;
        v.relayHost = relayHost_;
        v.stageRule = host_ ? StageRule() : stageRule_;
        v.overResults = state_ == State::Lobby && game_.BattleRunning();
        v.haveSetup = state_ >= State::Jumping && state_ <= State::PostMatch;
        v.setup = setup_;
        return v;
    }

    void Stop()
    {
        ReleaseResults();
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (jump_.joinable()) jump_.join();
        if (control_) SendBye();
        SetPadIsolation(false);
        game_.localInput = nullptr;
        game_.exclusiveTicks = false;
        SetRoundDirectorPerTick(false, false);
        // Left mid-match (leave, a new host/join, a reload): keep the recording.
        if (session_) {
            const std::string path = SaveReport(session_->Recording(), "match");
            Log("netplay: left at frame %d; recording %s", session_->NextFrame(), path.c_str());
            SetResult("left at frame " + std::to_string(session_->NextFrame()));
            session_.reset();
        }
        spectate_.reset();
        SetBlockPause(false);
        SetScriptedPad(0, false);
        SetScriptedPad(1, false);
        ClearPendingSetup();
        // The room goes with the service (it leaves on the way out).
        control_ = gekko_ = nullptr;
        mux_.reset();
        conditioned_.reset();
        base_.reset();
        weyve_ = nullptr;
    }

    // Game thread, once per rendered battle frame.
    bool OnFrame(SfvGame& game) override
    {
        // The lobby over the last match's results screen: the battle stays
        // as it is (its own results menu would open if it ran on).
        if (holdOverResults_ && !matchLive_) return true;
        if (next_ != Next::None && matchLive_) {
            // A spectator watches the match to its end first, unless the
            // session itself ended.
            const bool watchOn = role_ == Role::Spectator && spectate_ && next_ != Next::End && !MatchDecided();
            if (!watchOn) {
                FinishMatch(game);
                return false;
            }
        }
        const State s = GetState();
        if (s == State::Jumping || s == State::Loading) return FrameWhileLoading(game);
        if (s == State::Running || s == State::PostMatch) {
            if (spectate_) return FrameWhileWatching(game);
            if (session_) return FrameWhileRunning(game);
        }
        return false;
    }

    // Preloading: this player's pick, and once known the other's.
    std::vector<PreloadPick> PreloadPicks()
    {
        std::vector<PreloadPick> picks;
        std::lock_guard lock(mutex_);
        if (state_ != State::Lobby && state_ != State::Hosting && state_ != State::Joining &&
            state_ != State::Connecting && state_ != State::Ended) {
            return picks;
        }
        if (options_.randomSetup) return picks;
        auto add = [&](bool has, const PlayerSetup& p) {
            if (has && p.character) picks.push_back({UnpackCode(p.character), static_cast<int>(p.costume)});
        };
        if (role_ == Role::Player) add(options_.hasPick, options_.pick);
        for (int i = 0; i < kNumPlayers; i++) {
            if (role_ == Role::Player && i == (host_ ? 0 : 1)) continue;
            add(seats_[i].present && seats_[i].hasPick, seats_[i].pick);
        }
        return picks;
    }

private:
    uint32_t Now() const
    {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch_).count());
    }

    // ----- control channel (net thread) --------------------------------------

    void StartThread()
    {
        stop_ = false;
        thread_ = std::thread([this] {
            ThreadGuard running;
            while (!stop_ && !Stopping()) {
                ControlTick();
                Sleep(5);
            }
        });
    }

    void SendTo(uint64_t to, lobby::Message m)
    {
        m.protocol = lobby::kProtocol;
        const auto bytes = lobby::Encode(m);
        control_->Send(to, bytes.data(), bytes.size());
    }

    void Send(lobby::Message m) { SendTo(peer_, std::move(m)); }

    // Every member of the session but this side: the other player (as seen
    // from here) and, from the host, the spectators.
    std::vector<uint64_t> Members()
    {
        std::vector<uint64_t> out;
        if (peer_) out.push_back(peer_);
        std::lock_guard lock(mutex_);
        for (const auto& [id, w] : watchers_) out.push_back(id);
        return out;
    }

    void SendBye()
    {
        lobby::Message bye;
        bye.type = lobby::Type::Bye;
        for (uint64_t to : Members()) SendTo(to, bye);
    }

    void HostReady(const std::string& code)
    {
        {
            std::lock_guard lock(mutex_);
            code_ = code;
        }
        CopyToClipboard(code);
        SetState(State::Hosting);
        Log("netplay: hosting, code %s (copied to the clipboard)", code.c_str());
    }

    void BeginHello()
    {
        helloSince_ = Clock::now();
        SetState(State::Joining);
        Log("netplay: joining %s", code_.c_str());
    }

    void ControlTick()
    {
        if (weyve_) RoomEvents();
        Packet p;
        while (control_->Receive(p)) {
            auto msg = lobby::Decode(p.data.data(), p.data.size());
            if (msg) OnMessage(p.from, *msg);
        }
        if (host_) StreamToWatchers();
        Transitions();

        const auto now = Clock::now();
        if (now - lastBeat_ < kBeat) return;
        lastBeat_ = now;

        // Tell the others this side gave up on the session.
        if (byePending_.exchange(false)) SendBye();
        const State s = GetState();
        if (s == State::Ended || s == State::Idle) return;
        if (s == State::Connecting && now - connectingSince_ > kConnectTimeout) {
            EndSession(weyve_ && weyve_->View().server != WeyveRoomView::Server::Connected
                           ? "cannot reach the server " + options_.server
                           : "no answer from the room");
            return;
        }
        if (CheckSilence(now)) return;
        UpdateLinkPath();

        if (s == State::Joining) {
            if (now - helloSince_ > kJoinTimeout) {
                EndSession("could not reach the host");
                return;
            }
            lobby::Message hello;
            hello.type = lobby::Type::Hello;
            hello.nonce = nonce_;
            hello.build = build_;
            hello.role = role_;
            hello.name = name_;
            Send(hello);
            return;
        }
        if (host_) HostBeat(s);
        else GuestBeat(s);
    }

    // ----- Weyvelength room events --------------------------------------------

    void RoomEvents()
    {
        WeyveEvent e;
        while (weyve_->NextEvent(e)) {
            switch (e.type) {
            case WeyveEvent::Type::RoomJoined:
                if (host_) {
                    weyve_->SetRoomData("build", Hex64(build_));
                    weyve_->SetRoomData("protocol", std::to_string(lobby::kProtocol));
                    MatchCode mc;
                    mc.kind = MatchCode::Kind::Room;
                    mc.room = e.text;
                    HostReady(EncodeMatchCode(mc));
                }
                else {
                    joinedRoom_ = true;
                }
                break;
            case WeyveEvent::Type::RoomError:
                if (GetState() == State::Connecting) EndSession(host_ ? "cannot create a room: " + e.text : "cannot join: " + e.text);
                break;
            case WeyveEvent::Type::PeerLeft:
                if (e.peer == peer_) {
                    if (GetState() != State::Ended) {
                        Log("netplay: the other %s left the room", host_ || role_ == Role::Player ? "player" : "side");
                        peerLeft_ = true;
                        EndSession(host_ || role_ == Role::Player ? "the other player left" : "the host left");
                    }
                }
                else {
                    std::lock_guard lock(mutex_);
                    if (watchers_.erase(e.peer)) Log("netplay: spectator %u left", static_cast<unsigned>(e.peer));
                }
                break;
            case WeyveEvent::Type::Removed:
            case WeyveEvent::Type::ServerLost: {
                const State s = GetState();
                if (s == State::Ended) break;
                // A match in progress runs on the P2P link; let it play out
                // if the link survives (it does not yet: Weyvelength drops
                // every link with the server connection).
                if (s == State::Running || s == State::PostMatch || s == State::Loading || s == State::Jumping) {
                    Log("netplay: %s; the match goes on as long as the link holds", e.text.c_str());
                    serverLost_ = true;
                    break;
                }
                EndSession(e.text);
                break;
            }
            case WeyveEvent::Type::PeerJoined:
                break;
            }
        }
        // Joiner: in the room; check its build before saying hello.
        if (joinedRoom_ && GetState() == State::Connecting) {
            const WeyveRoomView v = weyve_->View();
            const auto build = v.data.find("build");
            if (v.host && build != v.data.end()) {
                joinedRoom_ = false;
                if (build->second != Hex64(build_)) {
                    Log("netplay: the room's build is %s, this one %s", build->second.c_str(), Hex64(build_).c_str());
                    EndSession("rejected: different mod build");
                    return;
                }
                peer_ = v.host;
                BeginHello();
            }
        }
    }

    // ----- heartbeats ---------------------------------------------------------

    // Peer to peer or through a TURN relay, for the panel and HUD; logged
    // when it changes (ICE can move a link to a better pair).
    void UpdateLinkPath()
    {
        if (!weyve_ || !peer_) return;
        NetView::Path path = NetView::Path::Unknown;
        const auto askedAt = Clock::now();
        switch (weyve_->PeerPath(static_cast<uint32_t>(peer_))) {
        case WeyveService::LinkPath::Direct: path = NetView::Path::Direct; break;
        case WeyveService::LinkPath::Relayed: path = NetView::Path::Relayed; break;
        default: break;
        }
        const std::string relay = weyve_->TurnHost();
        // It holds the client's lock, which the game thread's sends wait on.
        const auto took = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - askedAt).count();
        if (took >= 2000) Log("hitch: the link path query took %.1f ms", took / 1000.0);
        std::lock_guard lock(mutex_);
        if (path != linkPath_ && path != NetView::Path::Unknown) {
            Log("netplay: the link to the other side is %s", path == NetView::Path::Relayed ? ("relayed through " + (relay.empty() ? std::string("TURN") : relay)).c_str() : "peer to peer");
        }
        linkPath_ = path;
        relayHost_ = relay;
    }

    // Ends the session (or drops a spectator) when nothing was heard for a
    // while. True if the session ended.
    bool CheckSilence(Clock::time_point now)
    {
        const State s = GetState();
        const bool waiting = s == State::Connecting || s == State::Hosting || s == State::Joining;
        if (!waiting && peer_ && now - heard_ > kSilence) {
            Log("netplay: nothing from the other side for %lld s",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - heard_).count()));
            EndSession("connection lost");
            return true;
        }
        if (host_) {
            std::lock_guard lock(mutex_);
            for (auto it = watchers_.begin(); it != watchers_.end();) {
                if (now - it->second.heard > kSilence) {
                    Log("netplay: spectator %s silent; dropped", it->second.name.c_str());
                    it = watchers_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        return false;
    }

    void Heard(uint32_t time)
    {
        heard_ = Clock::now();
        heardTime_ = time;
        heardTimeAt_ = heard_;
    }

    void Echo(lobby::Message& m, uint32_t heardTime, Clock::time_point heardAt)
    {
        m.time = Now();
        m.echo = heardTime;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - heardAt).count();
        m.echoAge = static_cast<uint16_t>(std::clamp<long long>(age, 0, 65535));
    }

    void TakeEcho(const lobby::Message& m)
    {
        if (!m.echo) return;
        const int64_t rtt = int64_t(Now()) - int64_t(m.echo) - m.echoAge;
        if (rtt >= 0 && rtt < 10000) linkPingMs_ = static_cast<unsigned>(rtt);
    }

    lobby::Seat OwnSeat()
    {
        lobby::Seat seat;
        std::lock_guard lock(mutex_);
        seat.present = true;
        seat.name = name_;
        seat.hasPick = options_.hasPick;
        seat.pick = options_.pick;
        seat.ready = ready_;
        seat.preloaded = PreloadsDone();
        seat.choice = myChoice_;
        seat.inputDelay = nextDelay_ >= 0 ? static_cast<uint8_t>(nextDelay_.load()) : options_.inputDelay;
        seat.fps = static_cast<uint8_t>(std::clamp(RenderFps() + 0.5f, 0.0f, 255.0f));
        seat.link = link_;
        return seat;
    }

    uint16_t StageRule()
    {
        if (hostStageRule_ >= 0) return static_cast<uint16_t>(hostStageRule_.load());
        if (options_.stageCode == "random") return lobby::kRandomStage;
        if (!options_.stageCode.empty()) {
            const int i = StageIndex(options_.stageCode);
            return i >= 0 ? static_cast<uint16_t>(i) : lobby::kAnyStage;
        }
        return options_.stage; // a table index, or kKeepStage (= kAnyStage)
    }

    void HostBeat(State s)
    {
        lobby::Message st;
        st.type = lobby::Type::SessionState;
        {
            const lobby::Seat own = OwnSeat();
            std::lock_guard lock(mutex_);
            const uint16_t wins = seats_[0].wins;
            seats_[0] = own;
            seats_[0].wins = wins;
            st.seats[0] = seats_[0];
            st.seats[1] = seats_[1];
            st.played = static_cast<uint16_t>(played_);
            st.spectators = static_cast<uint8_t>(watchers_.size());
        }
        st.phase = s == State::PostMatch ? SessionPhase::PostMatch
                   : s >= State::Jumping && s <= State::Running ? SessionPhase::Match
                                                                : SessionPhase::Lobby;
        st.matchId = matchId_;
        st.stage = StageRule();
        if (peer_) {
            Echo(st, heardTime_, heardTimeAt_);
            Send(st);
        }
        std::map<uint64_t, Watcher> watchers;
        {
            std::lock_guard lock(mutex_);
            watchers = watchers_;
        }
        for (const auto& [id, w] : watchers) {
            Echo(st, w.heardTime, w.heardTimeAt);
            SendTo(id, st);
        }

        // A match: Welcome to whoever has not loaded it yet (a spectator still
        // watching an earlier match takes it up once that one has ended),
        // then Loaded once frozen.
        const bool matchOn = s == State::Jumping || s == State::Loading || s == State::Running;
        if (matchOn && peer_ && !peerLoaded_) Send(WelcomeMessage());
        for (const auto& [id, w] : watchers) {
            if ((matchOn || s == State::PostMatch) && w.match != matchId_) SendTo(id, WelcomeMessage());
        }
        if (selfLoaded_ && (s == State::Loading || s == State::Running || s == State::PostMatch)) {
            const lobby::Message loaded = LoadedMessage();
            if (s == State::Loading || sessionSyncing_) Send(loaded);
            for (const auto& [id, w] : watchers) {
                if (w.match == matchId_ && !w.streaming) SendTo(id, loaded);
            }
        }
        if (s == State::Loading || s == State::Running) SendReconcile(watchers);

        if (s == State::Lobby) MaybeStartMatch();
        if (s == State::PostMatch) MaybeResolve();
    }

    void GuestBeat(State s)
    {
        if (role_ == Role::Player) {
            lobby::Message ps;
            ps.type = lobby::Type::PlayerState;
            ps.seat = OwnSeat();
            Echo(ps, heardTime_, heardTimeAt_);
            Send(ps);
        }
        else {
            // A spectator's heartbeat: what it needs of the stream.
            lobby::Message ack;
            ack.type = lobby::Type::InputsAck;
            ack.matchId = watchingMatch_;
            ack.first = static_cast<uint32_t>(std::max(0, specWanted_.load()));
            ack.time = Now();
            Send(ack);
        }
        // Loaded is repeated until the host has started too (it may be lost).
        if ((s == State::Loading && selfLoaded_) || (s == State::Running && sessionSyncing_)) Send(LoadedMessage());
        if (s == State::Loading) SendReconcile({});
    }

    lobby::Message WelcomeMessage()
    {
        lobby::Message w;
        w.type = lobby::Type::Welcome;
        w.matchId = matchId_;
        w.setup = setup_;
        w.inputDelay = options_.inputDelay;
        w.predictionWindow = options_.predictionWindow;
        return w;
    }

    lobby::Message LoadedMessage()
    {
        lobby::Message loaded;
        loaded.type = lobby::Type::Loaded;
        loaded.matchId = matchId_;
        loaded.setup = loadedSetup_;
        loaded.stateChecksum = frozenChecksum_;
        loaded.reconciled = reconciled_;
        return loaded;
    }

    // ----- messages ------------------------------------------------------------

    void OnMessage(uint64_t from, const lobby::Message& m)
    {
        const State s = GetState();
        if (m.protocol != lobby::kProtocol) {
            if (host_ && m.type == lobby::Type::Hello) Reject(from, lobby::RejectReason::ProtocolMismatch);
            return;
        }
        if (from == peer_ && s != State::Joining) heard_ = Clock::now();
        if (host_) OnHostMessage(from, m, s);
        else OnGuestMessage(from, m, s);
    }

    void OnHostMessage(uint64_t from, const lobby::Message& m, State s)
    {
        Watcher* w = nullptr;
        std::unique_lock lock(mutex_);
        if (auto it = watchers_.find(from); it != watchers_.end()) {
            w = &it->second;
            w->heard = Clock::now();
        }
        lock.unlock();
        switch (m.type) {
        case lobby::Type::Hello: {
            if (options_.udp && m.nonce != nonce_) {
                Reject(from, lobby::RejectReason::WrongCode);
                break;
            }
            if (m.build != build_) {
                Reject(from, lobby::RejectReason::BuildMismatch);
                break;
            }
            lobby::Message accept;
            accept.type = lobby::Type::Accept;
            accept.name = name_;
            if (from == peer_) {
                accept.role = Role::Player;
                SendTo(from, accept);
                break;
            }
            if (w) {
                accept.role = Role::Spectator;
                SendTo(from, accept);
                break;
            }
            if (m.role == Role::Player && !peer_ && s != State::Ended) {
                peer_ = from;
                heard_ = Clock::now();
                {
                    std::lock_guard l(mutex_);
                    seats_[1] = {};
                    seats_[1].present = true;
                    seats_[1].name = m.name;
                }
                Log("netplay: %s joined from %s", m.name.c_str(), Hex64(from).c_str());
                if (s == State::Hosting) SetState(State::Lobby);
                accept.role = Role::Player;
                SendTo(from, accept);
                break;
            }
            std::lock_guard l(mutex_);
            if (!options_.allowSpectators || watchers_.size() >= kMaxSpectators) {
                Reject(from, options_.allowSpectators ? lobby::RejectReason::Busy : lobby::RejectReason::NoSpectators);
                break;
            }
            Watcher& nw = watchers_[from];
            nw.name = m.name;
            nw.heard = Clock::now();
            Log("netplay: %s watches (from %s)", m.name.c_str(), Hex64(from).c_str());
            accept.role = Role::Spectator;
            SendTo(from, accept);
            break;
        }
        case lobby::Type::PlayerState:
            if (from != peer_) break;
            Heard(m.time);
            TakeEcho(m);
            {
                std::lock_guard l(mutex_);
                lobby::Seat& seat = seats_[1];
                seat.hasPick = m.seat.hasPick;
                seat.pick = m.seat.pick;
                // A joiner still in the post-match screen (its choice made) is
                // not ready for the next match: its last heartbeats before it
                // takes the lobby carry the ready flag of the match before,
                // and started the next match at once.
                seat.ready = m.seat.ready && m.seat.choice == Choice::None;
                seat.preloaded = m.seat.preloaded;
                seat.choice = m.seat.choice;
                seat.inputDelay = m.seat.inputDelay;
                seat.fps = m.seat.fps;
                seat.link = m.seat.link;
            }
            break;
        case lobby::Type::Loaded:
            if (from == peer_ && m.matchId == matchId_) {
                {
                    std::lock_guard l(mutex_);
                    peerLoadedSetup_ = m.setup;
                    peerChecksum_ = m.stateChecksum;
                }
                peerReconciled_ = m.reconciled;
                peerLoaded_ = true;
            }
            else if (w && m.matchId == matchId_) {
                std::lock_guard l(mutex_);
                if (w->match != m.matchId) {
                    w->streaming = false;
                    w->wanted = 0;
                    w->sentThrough = -1;
                }
                w->match = m.matchId;
                w->loaded = true;
                w->checksum = m.stateChecksum;
                w->reconciled = m.reconciled;
            }
            break;
        case lobby::Type::StateRequest:
            if ((from == peer_ || w) && m.matchId == matchId_) OnStateRequest(from, m);
            break;
        case lobby::Type::InputsAck:
            if (w) {
                std::lock_guard l(mutex_);
                w->heardTime = m.time;
                w->heardTimeAt = Clock::now();
                if (!m.matchId) {
                    // Done watching (or not yet watching anything).
                    if (w->streaming) Log("netplay: spectator %s finished watching", w->name.c_str());
                    w->streaming = false;
                }
                else if (streams_.count(m.matchId)) {
                    if (!w->streaming || w->match != m.matchId) {
                        w->streaming = true;
                        w->match = m.matchId;
                        w->sentThrough = static_cast<int>(m.first) - 1;
                        w->progressAt = Clock::now();
                        Log("netplay: spectator %s is watching from frame %u", w->name.c_str(), m.first);
                    }
                    if (static_cast<int>(m.first) > w->wanted) {
                        w->wanted = static_cast<int>(m.first);
                        w->progressAt = Clock::now();
                    }
                }
            }
            break;
        case lobby::Type::Bye:
            if (from == peer_ && s != State::Ended && s != State::Idle) {
                Log("netplay: the other player left");
                peerLeft_ = true;
                EndSession("the other player left");
            }
            else if (w) {
                std::lock_guard l(mutex_);
                Log("netplay: spectator %s left", w->name.c_str());
                watchers_.erase(from);
            }
            break;
        default:
            break;
        }
    }

    void OnGuestMessage(uint64_t from, const lobby::Message& m, State s)
    {
        if (from != peer_) return;
        switch (m.type) {
        case lobby::Type::Accept:
            if (s != State::Joining) break;
            heard_ = Clock::now();
            role_ = m.role;
            {
                std::lock_guard l(mutex_);
                seats_[0].name = m.name;
            }
            Log("netplay: in %s's session as %s", m.name.c_str(), role_ == Role::Player ? "P2" : "a spectator");
            if (role_ == Role::Spectator && options_.mashSeed) options_.mashSeed = 0;
            SetState(State::Lobby);
            break;
        case lobby::Type::Reject:
            if (s == State::Joining) {
                Log("netplay: rejected: %s", RejectText(m.reason));
                EndSession(std::string("rejected: ") + RejectText(m.reason));
            }
            break;
        case lobby::Type::SessionState: {
            Heard(m.time);
            TakeEcho(m);
            {
                std::lock_guard l(mutex_);
                for (int i = 0; i < kNumPlayers; i++) seats_[i] = m.seats[i];
                played_ = m.played;
                spectators_ = m.spectators;
                stageRule_ = m.stage;
            }
            // The host went back to the lobby (character select).
            if (m.phase == SessionPhase::Lobby && s >= State::Jumping && s <= State::PostMatch && next_ == Next::None) {
                Log("netplay: back to the lobby");
                next_ = Next::Lobby;
            }
            break;
        }
        case lobby::Type::Welcome: {
            if (m.matchId == matchId_ || s == State::Joining || s == State::Connecting || s == State::Ended) break;
            // A new match: from the lobby straight away, from a match once the
            // game thread has let go of it.
            {
                std::lock_guard l(mutex_);
                setup_ = m.setup;
            }
            matchId_ = m.matchId;
            // Each player keeps its own input delay; the prediction window is fixed.
            options_.predictionWindow = kPredictionWindow;
            Log("netplay: match %s; %s", Hex64(matchId_).c_str(), DescribeSetup(setup_).c_str());
            next_ = Next::Jump;
            break;
        }
        case lobby::Type::Loaded:
            // Repeated, and the host's checksum is final.
            if (m.matchId == matchId_) {
                {
                    std::lock_guard l(mutex_);
                    peerLoadedSetup_ = m.setup;
                    peerChecksum_ = m.stateChecksum;
                }
                peerLoaded_ = true;
            }
            break;
        case lobby::Type::StateHashes:
            if (m.matchId == matchId_) OnStateHashes(m);
            break;
        case lobby::Type::StateData:
            if (m.matchId == matchId_) OnStateData(m);
            break;
        case lobby::Type::Inputs:
            if (role_ == Role::Spectator && m.matchId && m.matchId == watchingMatch_) {
                std::lock_guard l(mutex_);
                inbound_.push_back(m);
            }
            break;
        case lobby::Type::Bye:
            if (s != State::Ended && s != State::Idle) {
                Log("netplay: the host left");
                peerLeft_ = true;
                EndSession(role_ == Role::Player ? "the other player left" : "the host left");
            }
            break;
        default:
            break;
        }
    }

    void Reject(uint64_t to, lobby::RejectReason reason)
    {
        lobby::Message m;
        m.type = lobby::Type::Reject;
        m.reason = reason;
        SendTo(to, m);
    }

    // ----- the session's course (net thread) --------------------------------

    // Host, in the lobby: both seats ready (and their characters loaded, or
    // waited for long enough).
    void MaybeStartMatch()
    {
        lobby::Seat a, b;
        {
            std::lock_guard lock(mutex_);
            a = seats_[0];
            b = seats_[1];
        }
        if (!b.present || !a.ready || !b.ready) {
            bothReadyAt_ = {};
            return;
        }
        const auto now = Clock::now();
        if (bothReadyAt_ == Clock::time_point{}) bothReadyAt_ = now;
        if (!(a.preloaded && b.preloaded) && now - bothReadyAt_ < kPreloadWait) return;
        if (!(a.preloaded && b.preloaded)) Log("netplay: starting without both characters preloaded");
        bothReadyAt_ = {};
        StartMatch();
    }

    void StartMatch()
    {
        MatchSetup setup = ChooseSetup();
        {
            std::lock_guard lock(mutex_);
            // The session's score so far, for the game's own counters.
            for (int i = 0; i < kNumPlayers; i++) {
                setup.wins[i] = seats_[i].wins;
                setup.streaks[i] = streaks_[i];
            }
            setup_ = setup;
        }
        matchId_ = rng_();
        Log("netplay: match %s; %s", Hex64(matchId_).c_str(), DescribeSetup(setup_).c_str());
        next_ = Next::Jump;
    }

    // Host, after a match: both chose.
    void MaybeResolve()
    {
        if (next_ != Next::None) return;
        Choice theirs;
        {
            std::lock_guard lock(mutex_);
            theirs = seats_[1].choice;
        }
        const Choice mine = myChoice_;
        if (mine == Choice::None || theirs == Choice::None) return;
        if (mine == Choice::Rematch && theirs == Choice::Rematch) {
            Log("netplay: both chose a rematch");
            StartMatch();
            return;
        }
        Log("netplay: back to the lobby");
        next_ = Next::Lobby;
    }

    // Acts on next_ once the game thread has let go of the match.
    void Transitions()
    {
        const Next next = next_;
        if (next == Next::None) return;
        if (matchLive_) {
            // No battle frames come while jumping from the menus: the match
            // can be let go of here.
            if (GetState() != State::Jumping || game_.BattleRunning()) return;
            SetFrameDriver(nullptr);
            Sleep(50);
            FinishMatch(game_);
        }
        next_ = Next::None;
        switch (next) {
        case Next::Jump:
            BeginJump();
            break;
        case Next::Lobby:
            myChoice_ = Choice::None;
            ready_ = options_.autoReady;
            {
                std::lock_guard lock(mutex_);
                for (auto& seat : seats_) {
                    seat.choice = Choice::None;
                    seat.ready = false;
                }
            }
            SetState(State::Lobby);
            // Character select after a match: the picks change over the
            // results screen (held), and the next match jumps from there as a
            // rematch does, without a load of the menus in between.
            if (game_.BattleRunning()) {
                SetHoldResults(true);
                holdOverResults_ = true;
                SetFrameDriver(this);
                // Nothing the players press reaches the game meanwhile: a
                // press on the results screen opens its own Results Menu.
                SetPadIsolation(true);
                SetBlockPause(true);
            }
            break;
        case Next::End:
            ReleaseResults();
            SetState(State::Ended);
            // Staying in the battle: let its results screen open.
            if (!options_.menuAfterMatch) SetHoldResults(false);
            if (options_.menuAfterMatch && game_.BattleRunning()) ReturnToMainMenu(kMenuAfterMatchFrames);
            if (weyve_) weyve_->LeaveRoom();
            break;
        case Next::None:
            break;
        }
    }

    void EndSession(const std::string& why)
    {
        // Once: the room's leave notice and the peer's Bye usually both arrive.
        if (GetState() == State::Ended || next_ == Next::End) return;
        Log("netplay: session over: %s", why.c_str());
        SetStatus(why);
        SetResult(why);
        if (!peerLeft_) byePending_ = true;
        if (!peer_ || GetState() < State::Lobby) {
            // Nothing begun: no match to let go of.
            SetState(State::Ended);
            if (weyve_) weyve_->LeaveRoom();
            return;
        }
        next_ = Next::End;
    }

    MatchSetup ChooseSetup()
    {
        MatchSetup s;
        if (options_.randomSetup) {
            for (auto& p : s.players) {
                p.character = PackCode(kRandomCharacters[rng_() % std::size(kRandomCharacters)]);
                p.costume = options_.randomCostumes ? static_cast<uint16_t>(rng_() % 3) : 0;
                p.color = static_cast<uint8_t>(rng_() % 10);
                p.vskill = static_cast<uint8_t>(rng_() % 2);
                p.vtrigger = static_cast<uint8_t>(rng_() % 2);
                p.comment = 0;
            }
            std::vector<uint16_t> stages;
            for (int i = 0; i < StageCount(); i++) {
                if (IsSelectableStage(i)) stages.push_back(static_cast<uint16_t>(i));
            }
            s.stage = stages.empty() ? kKeepStage : stages[rng_() % stages.size()];
        }
        else {
            MatchSetup current;
            ReadMatchSetup(&current);
            lobby::Seat joiner;
            {
                std::lock_guard lock(mutex_);
                joiner = seats_[1];
                s.players[0] = options_.hasPick ? options_.pick : current.players[0];
            }
            s.players[1] = joiner.hasPick ? joiner.pick : current.players[1];
            const uint16_t rule = StageRule();
            s.stage = rule == lobby::kAnyStage ? kKeepStage : rule;
            if (rule == lobby::kRandomStage) {
                std::vector<int> playable;
                for (int i = 0; i < StageCount(); i++) {
                    if (IsSelectableStage(i)) playable.push_back(i);
                }
                s.stage = playable.empty() ? kKeepStage : static_cast<uint16_t>(playable[rng_() % playable.size()]);
            }
        }
        s.seed = rng_();
        return s;
    }

    // ----- spectators' stream (host, net thread) ----------------------------

    void StreamToWatchers()
    {
        const auto now = Clock::now();
        std::vector<std::pair<uint64_t, lobby::Message>> out;
        {
            std::lock_guard lock(mutex_);
            if (watchers_.empty()) return;
            for (auto& [id, w] : watchers_) {
                if (!w.streaming) continue;
                const auto found = streams_.find(w.match);
                if (found == streams_.end()) continue;
                const std::vector<lobby::FrameRecord>& stream = found->second;
                const int have = static_cast<int>(stream.size());
                // No progress for a while: what was sent got lost; again from
                // what it has.
                if (now - w.progressAt > kStreamResend && w.sentThrough >= w.wanted) {
                    w.sentThrough = w.wanted - 1;
                    w.progressAt = now;
                }
                const int limit = w.wanted + kStreamBatchesAhead * static_cast<int>(lobby::kInputsPerMessage);
                while (w.sentThrough + 1 < have && w.sentThrough + 1 < limit) {
                    lobby::Message m;
                    m.type = lobby::Type::Inputs;
                    m.matchId = w.match;
                    m.first = static_cast<uint32_t>(w.sentThrough + 1);
                    const int end = std::min({have, limit, w.sentThrough + 1 + static_cast<int>(lobby::kInputsPerMessage)});
                    m.frames.assign(stream.begin() + (w.sentThrough + 1), stream.begin() + end);
                    w.sentThrough = end - 1;
                    out.emplace_back(id, std::move(m));
                }
            }
        }
        for (auto& [id, m] : out) SendTo(id, m);
    }

    // ----- getting into the battle -------------------------------------------

    // The lobby's hold on the results screen ends (a match starts from there,
    // or the session ends).
    void ReleaseResults()
    {
        if (!holdOverResults_.exchange(false)) return;
        SetPadIsolation(false);
        SetBlockPause(false);
        SetFrameDriver(nullptr);
        Sleep(50); // the game thread lets go of this driver's frame
    }

    void BeginJump()
    {
        ReleaseResults();
        // A delay chosen between matches takes effect with this one.
        if (nextDelay_ >= 0) options_.inputDelay = static_cast<uint8_t>(nextDelay_.exchange(-1));
        SetPendingSetup(setup_);
        creationsAtJump_ = BattleCreationCount();
        warmedUp_ = false;
        frozenFrames_ = 0;
        selfLoaded_ = false;
        peerLoaded_ = false;
        peerReconciled_ = false;
        reconciled_ = false;
        hostDataComplete_ = false;
        bothLoadedAt_ = {};
        myChoice_ = Choice::None;
        specWanted_ = 0;
        {
            std::lock_guard lock(mutex_);
            requested_ = false;
            hostHashes_.clear();
            hostHashParts_.clear();
            hostRegions_.clear();
            hostRegionChunks_.clear();
            inbound_.clear();
            for (auto& seat : seats_) seat.choice = Choice::None;
            live_ = NetView{};
        }
        watchingMatch_ = 0;
        liveMatch_ = matchId_.load();
        if (jump_.joinable()) jump_.join();
        matchLive_ = true;
        SetState(State::Jumping);
        SetFrameDriver(this);
        jump_ = std::thread([this] {
            ThreadGuard running;
            RunJump();
        });
    }

    void RunJump()
    {
        // Straight into the battle scene with the setup (flow.h), from the
        // menus, a running battle or its results.
        SetPendingStage(setup_.stage == kKeepStage ? "" : StageCode(setup_.stage));
        Log("netplay: jumping into the battle (stage %s)", setup_.stage == kKeepStage ? "as prepared" : StageCode(setup_.stage).c_str());
        JumpToBattle();
        const auto direct = Clock::now() + std::chrono::seconds(kDirectJumpSeconds);
        while (!stop_ && !Stopping() && GetState() == State::Jumping && BattleCreationCount() == creationsAtJump_ &&
               Clock::now() < direct) {
            Sleep(100);
        }
        if (stop_ || Stopping() || GetState() != State::Jumping || BattleCreationCount() != creationsAtJump_) return;
        Log("netplay: no battle %d s after the jump", kDirectJumpSeconds);
        SetStatus("could not start the battle (see the log)");
        EndSession("could not start the battle (see the log)");
    }

    bool FrameWhileLoading(SfvGame& game)
    {
        // Still the battle we left: let it run.
        if (BattleCreationCount() == creationsAtJump_) return false;

        if (!warmedUp_) {
            game.ForgetBattleObjects();
            warmedUp_ = true;
            objectsReady_ = false;
            warmupTicks_ = 0;
            SetState(State::Loading);
            SetBlockPause(true);
            Log("netplay: battle created");
        }
        // The battle's units (and so its state regions) only show up after
        // some ticks of the new battle. Both machines run neutral ticks until
        // they exist; then the battle waits for the other player.
        if (!objectsReady_) {
            game.RefreshRegions();
            if (game.Regions().empty() || !game.InputUnit()) {
                game.Advance(FrameInputs{}, false);
                if (++warmupTicks_ % 300 == 0) Log("netplay: still no battle objects after %d ticks", warmupTicks_);
                return true;
            }
            objectsReady_ = true;
            // From here on the game sees neutral pads; the session feeds it.
            SetPadIsolation(true);
            game.exclusiveTicks = true;
            SetRoundDirectorPerTick(true, options_.roundEndRollback);
            SetMatchDrawSeed(setup_.seed);
            game.Recanonicalize();
            std::vector<uint64_t> hashes(game.Regions().size());
            HashRegions(game.Regions(), hashes.data());
            frozenChecksum_ = FoldChecksum(hashes.data(), hashes.size());
            KeepFrozenState(game.Regions(), hashes);
            ReadMatchSetup(&loadedSetup_);
            Log("netplay: battle objects ready after %d neutral ticks (%zu regions, checksum %08X); frozen",
                warmupTicks_, hashes.size(), frozenChecksum_);
            Log("netplay: loaded %s", DescribeSetup(loadedSetup_).c_str());
        }
        if (++frozenFrames_ == kSettleFrames) {
            selfLoaded_ = true;
            Log("netplay: loaded; waiting for the %s", role_ == Role::Spectator ? "host" : "other player");
        }
        SetStatus(peerLoaded_ ? "starting" : selfLoaded_ ? "waiting for the other player" : "loading");
        if (selfLoaded_ && peerLoaded_ && !session_ && !spectate_) {
            MatchSetup peer;
            uint32_t peerChecksum;
            {
                std::lock_guard lock(mutex_);
                peer = peerLoadedSetup_;
                peerChecksum = peerChecksum_;
            }
            const std::string mismatch = SetupMismatch(loadedSetup_, peer);
            if (!mismatch.empty()) {
                Log("netplay: the battles differ (%s); here %s | there %s", mismatch.c_str(),
                    DescribeSetup(loadedSetup_).c_str(), DescribeSetup(peer).c_str());
                EndSession("battles differ: " + mismatch);
                return true;
            }
            if (!ReadyToStart(game, peerChecksum)) return true;
            Log("netplay: frozen state checksums %08X here, %08X there%s", frozenChecksum_, peerChecksum,
                frozenChecksum_ == peerChecksum ? "" : " (differ)");
            if (role_ == Role::Spectator) StartWatching(game);
            else StartSession(game);
        }
        return true;
    }

    void StartSession(SfvGame& game)
    {
        NetplayConfig nc;
        nc.localPlayer = host_ ? 0 : 1;
        nc.remotePeer = peer_;
        nc.matchId = matchId_;
        nc.setup = setup_;
        nc.modVersion = modVersion_;
        nc.inputDelay = options_.inputDelay;
        nc.predictionWindow = options_.predictionWindow;
        nc.dumpEvery = options_.dumpEvery;
        nc.maxDumps = static_cast<size_t>(std::max(0, options_.maxDumps));
        nc.log = [](const std::string& m) { Log("netplay: %s", m.c_str()); };
        game.localPadSlot = 0;
        game.bothPadsLocal = false;
        game.localInput = &MappedLocalInput;
        session_ = std::make_unique<NetplaySession>(game, *gekko_, nc);
        session_->onDesync = [this](const DesyncInfo& d, const ReplayFile& rf) {
            desyncFrame_ = d.frame;
            const std::string path = SaveReport(rf, "desync");
            std::lock_guard lock(mutex_);
            desyncReport_ = path;
        };
        std::string error;
        if (!session_->Start(&error)) {
            Log("netplay: cannot start the session: %s", error.c_str());
            session_.reset();
            EndSession("could not start");
            return;
        }
        layoutAtStart_ = game.LayoutVersion();
        sessionSyncing_ = true;
        framesRun_ = 0;
        waitFrames_ = 0;
        waitCooldown_ = 0;
        desyncFrame_ = -1;
        {
            std::lock_guard lock(mutex_);
            desyncReport_.clear();
        }
        streamed_ = 0;
        predictionWindow_ = options_.predictionWindow;
        inputDelay_ = nc.inputDelay;
        roundCallFrame_ = -1;
        loweredEarly_ = false;
        pace_ = {};
        UpdatePredictionWindow();
        winsKnown_ = game_.MatchWins(winsAtStart_[0], winsAtStart_[1]);
        SetState(State::Running);
        Log("netplay: session started as P%d, delay %u, prediction window %u", nc.localPlayer + 1, nc.inputDelay,
            nc.predictionWindow);
    }

    void StartWatching(SfvGame& game)
    {
        SpectateConfig sc;
        sc.matchId = matchId_;
        sc.setup = setup_;
        sc.modVersion = modVersion_;
        sc.log = [](const std::string& m) { Log("netplay: %s", m.c_str()); };
        game.localInput = nullptr;
        spectate_ = std::make_unique<SpectatorPlayback>(game, sc);
        std::string error;
        if (!spectate_->Start(&error)) {
            Log("netplay: cannot watch: %s", error.c_str());
            spectate_.reset();
            EndSession("could not start");
            return;
        }
        layoutAtStart_ = game.LayoutVersion();
        framesRun_ = 0;
        waitFrames_ = 0;
        waitCooldown_ = 0;
        desyncFrame_ = -1;
        {
            std::lock_guard lock(mutex_);
            desyncReport_.clear();
        }
        watchingMatch_ = matchId_.load(); // its acks now ask the host for the stream
        SetState(State::Running);
        Log("netplay: watching match %s", Hex64(matchId_).c_str());
    }

    // ----- the match (game thread) -------------------------------------------

    bool FrameWhileRunning(SfvGame& game)
    {
        const State s = GetState();
        game.RefreshRegions();
        if (game.LayoutVersion() != layoutAtStart_ || game.Regions().empty()) {
            // The battle went away under the session.
            FinishMatch(game);
            if (s == State::Running) EndSession("match over");
            return false;
        }
        if (s == State::Running && (peerLeft_ || session_->Phase() == NetplayPhase::Ended)) {
            FinishMatch(game);
            EndSession("the other player left");
            return true;
        }

        // Mashing stops with the match's outcome: the post-match menu reads
        // the pads.
        if (options_.mashSeed && s == State::Running) Mash(session_->NextFrame());
        UpdatePredictionWindow();

        // Time sync: the side that runs ahead waits as many frames as it is
        // ahead. The figure reflects a wait only a round trip later, so the
        // next wait comes no sooner than that: waiting on every frame the
        // figure was still high made both sides wait in turn, about one frame
        // in five each at 100 ms (each wait also stalls the other side). In
        // lockstep the side ahead stalls for the other's inputs by itself.
        const float ahead = session_->FramesAhead();
        framesRun_++;
        if (waitCooldown_ > 0) waitCooldown_--;
        if (waitFrames_ == 0 && waitCooldown_ == 0 && predictionWindow_ > 0 && ahead >= 1.0f) {
            constexpr int kMaxWait = 4;
            const float rttMs = session_->NetworkStats().avg_ping;
            waitFrames_ = std::min(static_cast<int>(ahead), kMaxWait);
            waitCooldown_ = waitFrames_ + std::max(6, static_cast<int>(std::ceil(rttMs * 60 / 1000)));
        }
        const bool wait = waitFrames_ > 0;
        if (wait) waitFrames_--;
        // Only the side ahead corrects. Letting the side behind also run two
        // frames in one rendered frame (catch-up) made both sides swing: the
        // figure is noisy and a round trip late, so each in turn waited about
        // 100 frames in 600 and caught up 60, and the session ran at 54 frames
        // a second between two PCs that held 58 to 60 without it (2026-10-28).
        LogPace(ahead, wait);
        // The match over and its win screens played: the battle stays where
        // it is until both players agree on what follows (flow.h).
        if (!wait && !ResultsHeld()) session_->Update();
        sessionSyncing_ = session_->Phase() == NetplayPhase::Syncing;

        if (s == State::Running && MatchDecided()) {
            // The game counts each side's match wins; whose count went up
            // won. The host keeps the score (seat 0 is P1).
            uint32_t now[2] = {};
            const bool counted = winsKnown_ && game_.MatchWins(now[0], now[1]);
            const int winner = !counted ? -1 : now[0] > winsAtStart_[0] ? 0 : now[1] > winsAtStart_[1] ? 1 : -1;
            {
                std::lock_guard lock(mutex_);
                if (host_) {
                    played_++;
                    if (winner >= 0) {
                        seats_[winner].wins++;
                        streaks_[winner]++;
                        streaks_[1 - winner] = 0;
                    }
                }
            }
            if (winner >= 0) Log("netplay: P%d won the match", winner + 1);
            else Log("netplay: the match's winner is not known (win counts %s)", counted ? "unchanged" : "not found");
            SetState(State::PostMatch);
            SetHoldResults(true);
            if (options_.mashSeed) SetScriptedPad(0, true, 0);
            Log("netplay: the match is decided at frame %d", session_->NextFrame());
        }
        if (host_) Stream();
        Publish(ahead);
        return true;
    }

    // Host: confirmed frames into the spectators' stream.
    void Stream()
    {
        // Kept whether anyone watches or not: a spectator can join mid-match
        // and starts from frame 0.
        // A spectator behind watches a match to its end even after the
        // players moved on: the last few matches' streams are kept.
        const int through = session_->ConfirmedThrough();
        if (through < streamed_) return;
        std::lock_guard lock(mutex_);
        if (!streams_.count(liveMatch_)) {
            streamOrder_.push_back(liveMatch_);
            while (streamOrder_.size() > kStreamsKept) {
                streams_.erase(streamOrder_.front());
                streamOrder_.erase(streamOrder_.begin());
            }
        }
        std::vector<lobby::FrameRecord>& stream = streams_[liveMatch_];
        for (; streamed_ <= through; streamed_++) {
            lobby::FrameRecord f;
            if (!session_->ConfirmedFrame(streamed_, &f.inputs, &f.checksum)) break;
            stream.push_back(f);
        }
    }

    void Publish(float ahead)
    {
        // For the overlay: every frame for the counters, the network figures
        // every 10 frames.
        std::string status;
        std::unique_lock lock(mutex_);
        live_.syncing = sessionSyncing_;
        live_.frame = session_->NextFrame();
        live_.framesAhead = ahead;
        live_.desyncFrame = desyncFrame_;
        live_.inputDelay = inputDelay_;
        live_.predictionWindow = predictionWindow_;
        if (framesRun_ % 10 == 0) {
            const auto net = session_->NetworkStats();
            const auto rb = session_->RollbackStats();
            live_.pingMs = net.last_ping;
            live_.avgPingMs = net.avg_ping;
            live_.jitterMs = net.jitter;
            live_.rollbacks = rb.total_count;
            live_.longestRollback = rb.max_frames;
        }
        if (framesRun_ % 30 == 0) {
            const auto net = session_->NetworkStats();
            const auto rb = session_->RollbackStats();
            char buf[200];
            std::snprintf(buf, sizeof(buf), "P%d | frame %d | ping %u ms | rollbacks %u (max %u) | ahead %.1f%s",
                          host_ ? 1 : 2, session_->NextFrame(), net.last_ping, rb.total_count, rb.max_frames, ahead,
                          desyncFrame_ >= 0 ? (" | DESYNC at " + std::to_string(desyncFrame_)).c_str() : "");
            status = sessionSyncing_ ? "connecting to the other player" : buf;
        }
        lock.unlock();
        if (!status.empty()) SetStatus(status);
    }

    bool FrameWhileWatching(SfvGame& game)
    {
        game.RefreshRegions();
        if (game.LayoutVersion() != layoutAtStart_ || game.Regions().empty()) {
            FinishMatch(game);
            return false;
        }
        std::vector<lobby::Message> in;
        {
            std::lock_guard lock(mutex_);
            in.swap(inbound_);
        }
        for (const lobby::Message& m : in) {
            std::vector<FrameInputs> inputs;
            std::vector<uint32_t> sums;
            for (const auto& f : m.frames) {
                inputs.push_back(f.inputs);
                sums.push_back(f.checksum);
            }
            spectate_->Add(static_cast<int>(m.first), inputs, sums);
        }
        if (!ResultsHeld()) spectate_->Update();
        specWanted_ = spectate_->NextWanted();
        framesRun_++;
        if (spectate_->DesyncFrame() >= 0 && desyncFrame_ < 0) {
            desyncFrame_ = spectate_->DesyncFrame();
            const std::string path = SaveReport(spectate_->Recording(), "spectator-desync");
            std::lock_guard lock(mutex_);
            desyncReport_ = path;
        }
        if (GetState() == State::Running && MatchDecided()) {
            SetState(State::PostMatch);
            SetHoldResults(true);
            Log("netplay: the match is decided at frame %d", spectate_->NextFrame());
        }
        {
            std::lock_guard lock(mutex_);
            live_.frame = spectate_->NextFrame();
            live_.behind = spectate_->Buffered();
            live_.desyncFrame = desyncFrame_;
        }
        if (framesRun_ % 30 == 0) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "watching | frame %d | %d frames in hand%s", spectate_->NextFrame(),
                          spectate_->Buffered(),
                          desyncFrame_ >= 0 ? (" | DESYNC at " + std::to_string(desyncFrame_)).c_str() : "");
            SetStatus(buf);
        }
        return true;
    }

    // The intro, round call and round end are not rolled back with the battle
    // (and inputs can skip parts of them): predict only while the fight runs.
    // Lockstep advances at most the input delay's frames per one-way trip (2
    // frames per 100 ms ran the intro at 20 fps), so outside the fight the
    // local delay goes up to cover the trip; inputs matter little there
    // (Start skips once both pressed it). Back in the fight it drops to the
    // player's own. Lowering it makes GekkoNet skip local inputs for the
    // difference (those frames already have theirs), so it comes down before
    // "FIGHT!": the round call (flow event 5) lasts kRoundCallFrames, and the
    // delay drops as many frames before its end as it was raised, plus
    // kEarlyMargin, for the fight's first frames to take live inputs. The
    // last frames of the call then run in lockstep at the player's delay (a
    // little slower over a long trip, while nobody can act yet).
    static constexpr int kRoundCallFrames = 196; // measured: flow event 5 to 6 (3.267 s)
    static constexpr int kEarlyMargin = 2;

    void UpdatePredictionWindow()
    {
        const bool fight = !options_.lockstepPresentation || RoundInFight();
        const uint8_t window = fight ? options_.predictionWindow : 0;
        const int frame = session_->NextFrame();
        // The round call's first frame, and its length in the log (to keep
        // kRoundCallFrames right).
        if (RoundCallRunning()) {
            if (roundCallFrame_ < 0) roundCallFrame_ = frame;
        }
        else if (roundCallFrame_ >= 0) {
            if (fight) Log("netplay: the round call took %d frames", frame - roundCallFrame_);
            roundCallFrame_ = -1;
            loweredEarly_ = false;
        }
        uint8_t delay = options_.inputDelay;
        // Within the presentation the delay only goes up, until it comes down
        // ahead of "FIGHT!".
        if (!fight && options_.lockstepDelay) delay = std::max(window == predictionWindow_ ? inputDelay_ : delay, LockstepDelay());
        if (!fight && roundCallFrame_ >= 0 && delay > options_.inputDelay &&
            (loweredEarly_ || frame >= roundCallFrame_ + kRoundCallFrames - delay - kEarlyMargin)) {
            if (!loweredEarly_) {
                Log("netplay: input delay back to %u before FIGHT (round call frame %d, %d to go)", options_.inputDelay,
                    frame - roundCallFrame_, roundCallFrame_ + kRoundCallFrames - frame);
            }
            loweredEarly_ = true;
            delay = options_.inputDelay;
        }
        if (delay != inputDelay_) {
            session_->SetInputDelay(delay);
            Log("netplay: input delay %u from frame %d", delay, session_->NextFrame());
            inputDelay_ = delay;
        }
        if (window == predictionWindow_) return;
        session_->SetPredictionWindow(window);
        Log("netplay: prediction window %u from frame %d (%s)", window, session_->NextFrame(),
            window ? "fight" : "presentation");
        predictionWindow_ = window;
    }

    // Every 600 rendered frames: how many frames the session advanced, how
    // many of the rendered ones time sync spent waiting, how many the game's
    // window had the focus in (a game in the background renders slower when
    // the PC is busy with other things), and the network.
    void LogPace(float ahead, bool wait)
    {
        pace_.waits += wait;
        pace_.aheadSum += ahead;
        // Asks Windows only, no message to any window (safe on the game thread);
        // the real one: always_active makes the game's own call say it is in front.
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(RealForegroundWindow(), &foregroundPid);
        pace_.focused += foregroundPid == GetCurrentProcessId();
        if (++pace_.rendered < 600) return;
        const auto now = std::chrono::steady_clock::now();
        const int frame = session_->NextFrame();
        if (pace_.since.time_since_epoch().count() != 0) {
            const double s = std::chrono::duration<double>(now - pace_.since).count();
            const GekkoNetworkStats net = session_->NetworkStats();
            Log("netplay: pace %.1f frames/s (%d frames in %.1f s), %d of %d rendered frames waited, ahead %.2f, "
                "delay %u, window %u, ping %.0f ms, jitter %.0f ms, focused %d",
                (frame - pace_.frame) / s, frame - pace_.frame, s, pace_.waits, pace_.rendered,
                pace_.aheadSum / pace_.rendered, inputDelay_, predictionWindow_, net.avg_ping, net.jitter, pace_.focused);
        }
        pace_ = {};
        pace_.since = now;
        pace_.frame = frame;
    }

    // Frames that cover a one-way trip and its jitter, plus one, within the
    // player's own delay and kMaxLockstepDelay.
    uint8_t LockstepDelay()
    {
        constexpr int kMaxLockstepDelay = 12;
        const GekkoNetworkStats net = session_->NetworkStats();
        const float rttMs = std::max(net.avg_ping, static_cast<float>(linkPingMs_.load()));
        const int frames = static_cast<int>(std::ceil((rttMs / 2 + net.jitter) * 60 / 1000)) + 1;
        return static_cast<uint8_t>(std::clamp(frames, static_cast<int>(options_.inputDelay), kMaxLockstepDelay));
    }

    void Mash(int frame)
    {
        const uint64_t key[2] = {options_.mashSeed, static_cast<uint64_t>(frame / 5)};
        const uint64_t h = Hash64(key, sizeof(key));
        uint16_t b = 0;
        switch (h % 8) {
        case 0: case 1: b |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 2: case 3: b |= XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 4: b |= XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 5: b |= XINPUT_GAMEPAD_DPAD_UP; break;
        default: break;
        }
        static const uint16_t kAttack[] = {XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                           XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B};
        if ((h >> 8) % 3 == 0) b |= kAttack[(h >> 16) % 5];
        // Now and then V-Skill (MP+MK) or V-Trigger (HP+HK, RT is HK), so a
        // mashing match also runs those moves (noise mask coverage).
        uint8_t rt = 0;
        switch ((h >> 24) % 16) {
        case 0: b = XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B; break;
        case 1: b = XINPUT_GAMEPAD_RIGHT_SHOULDER; rt = 255; break;
        default: break;
        }
        if (frame < options_.mashFrom) b = 0, rt = 0;
        SetScriptedPad(0, true, b, rt);
    }

    // Lets go of the match: its recording saved, the game's own input back.
    // Game thread (or, while jumping without a battle, the net thread).
    void FinishMatch(SfvGame& game)
    {
        SetBlockPause(false);
        SetPadIsolation(false);
        game.localInput = nullptr;
        game.exclusiveTicks = false;
        SetRoundDirectorPerTick(false, false);
        SetScriptedPad(0, false);
        ClearPendingSetup();
        if (session_) {
            const auto rf = session_->Recording();
            const auto rb = session_->RollbackStats();
            const std::string path = SaveReport(rf, "match");
            Log("netplay: match %s: %d frames, %u rollbacks (max %u), %d desync events; recording %s",
                Hex64(liveMatch_).substr(0, 8).c_str(), session_->NextFrame(), rb.total_count, rb.max_frames,
                session_->DesyncCount(), path.c_str());
            session_.reset();
        }
        if (spectate_) {
            const std::string path = SaveReport(spectate_->Recording(), "watched");
            Log("netplay: watched %d frames (%d stalls, %d desyncs); recording %s", spectate_->NextFrame(),
                spectate_->Stalls(), spectate_->DesyncCount(), path.c_str());
            spectate_.reset();
        }
        watchingMatch_ = 0;
        SetFrameDriver(nullptr);
        matchLive_ = false;
    }

    std::string SaveReport(const ReplayFile& rf, const char* kind)
    {
        CreateDirectoryW(reportDir_.c_str(), nullptr);
        char name[96];
        const std::time_t t = std::time(nullptr);
        std::tm tm;
        localtime_s(&tm, &t);
        std::snprintf(name, sizeof(name), "%04d%02d%02d-%02d%02d%02d-%s-%s-%s.sfvr", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, Hex64(liveMatch_).substr(0, 8).c_str(),
                      role_ == Role::Spectator ? "spec" : host_ ? "p1" : "p2", kind);
        std::string dir;
        for (wchar_t c : reportDir_) dir += c < 128 ? static_cast<char>(c) : '_';
        const std::string path = dir + "\\" + name;
        std::string error;
        const ReplayDetail detail = options_.fullRecordings ? ReplayDetail::Full : ReplayDetail::Compact;
        if (!SaveReplay(rf, path, &error, detail)) Log("netplay: cannot save %s: %s", path.c_str(), error.c_str());
        else Log("netplay: saved %s", path.c_str());
        return path;
    }

    // ----- state -------------------------------------------------------------

    State GetState()
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

    void SetState(State s)
    {
        {
            std::lock_guard lock(mutex_);
            state_ = s;
        }
        if (s == State::Connecting) SetStatus("connecting to " + options_.server);
        else if (s == State::Hosting) SetStatus("HOST " + code_ + " (copied) - waiting for a player");
        else if (s == State::Joining) SetStatus("joining " + code_);
        else if (s == State::Lobby) SetStatus(role_ == Role::Spectator ? "watching: waiting for a match" : "lobby");
        else if (s == State::Jumping) SetStatus("jumping into battle");
        else if (s == State::PostMatch) SetStatus("match over: rematch or character select?");
    }

    void SetStatus(const std::string& text)
    {
        {
            std::lock_guard lock(mutex_);
            status_ = text;
        }
        sfv::mod::SetStatus(text);
    }

    SfvGame& game_;
    std::wstring reportDir_;
    std::string modVersion_;
    std::mt19937_64 rng_;
    const Clock::time_point epoch_;
    NetplayOptions options_; // pick fields guarded by mutex_
    bool host_ = false;
    std::atomic<Role> role_{Role::Player};
    std::string name_;
    lobby::Link link_ = lobby::Link::Unknown;

    std::unique_ptr<ITransport> base_;
    WeyveService* weyve_ = nullptr; // base_, when it is the Weyvelength client
    std::unique_ptr<ConditionedTransport> conditioned_;
    std::unique_ptr<Mux> mux_;
    ITransport* control_ = nullptr;
    ITransport* gekko_ = nullptr;
    uint64_t localId_ = 0;
    uint64_t build_ = 0;

    std::mutex mutex_;
    State state_ = State::Idle;
    std::string status_;
    std::string code_;
    std::atomic<uint64_t> peer_{0}; // the host, or (host) the other player
    uint16_t nonce_ = 0;
    std::atomic<uint64_t> matchId_{0};
    MatchSetup setup_;                     // guarded by mutex_ where both threads touch it
    lobby::Seat seats_[kNumPlayers];       // guarded by mutex_
    int played_ = 0;                       // guarded by mutex_
    std::atomic<bool> holdOverResults_{false}; // the lobby holds the last match's results screen (OnFrame)
    std::atomic<int> nextDelay_{-1};       // the input delay for the next match (-1: unchanged)
    std::atomic<int> hostStageRule_{-1};   // host: the stage rule set in the lobby (-1: from the options)
    uint16_t streaks_[kNumPlayers] = {};   // host: each player's current win streak this session
    uint32_t winsAtStart_[2] = {};         // the game's match win counts as the match started
    bool winsKnown_ = false;
    int spectators_ = 0;                   // guest: as the host says
    uint16_t stageRule_ = lobby::kAnyStage;
    std::map<uint64_t, Watcher> watchers_; // host; guarded by mutex_
    std::atomic<bool> ready_{false};
    std::atomic<Choice> myChoice_{Choice::None};
    std::atomic<Next> next_{Next::None};
    std::atomic<bool> matchLive_{false};   // the game thread drives a match (frame driver set)
    std::atomic<uint64_t> liveMatch_{0};   // ... this one (matchId_ moves on to the next before it is let go)
    bool joinedRoom_ = false;
    std::atomic<bool> serverLost_{false};
    Clock::time_point bothReadyAt_{};
    Clock::time_point helloSince_{};
    Clock::time_point connectingSince_{};

    // Heartbeats from the other side (guest: the host; host: the other player).
    Clock::time_point heard_ = Clock::now();
    uint32_t heardTime_ = 0;
    Clock::time_point heardTimeAt_{};
    std::atomic<unsigned> linkPingMs_{0};
    NetView::Path linkPath_ = NetView::Path::Unknown; // under mutex_
    int roundCallFrame_ = -1;   // game thread: the session frame the round call began, -1 outside it
    bool loweredEarly_ = false; // ... and the delay is already back to the player's
    std::string relayHost_;                            // under mutex_

    std::thread thread_;
    std::thread jump_;
    std::atomic<bool> stop_{false};
    Clock::time_point lastBeat_;

    int creationsAtJump_ = 0;
    bool warmedUp_ = false;
    int frozenFrames_ = 0;
    bool objectsReady_ = false;
    MatchSetup loadedSetup_;           // what this side's game loaded
    uint32_t frozenChecksum_ = 0;
    MatchSetup peerLoadedSetup_;       // guarded by mutex_
    uint32_t peerChecksum_ = 0;
    std::atomic<bool> byePending_{false};
    int warmupTicks_ = 0;
    std::atomic<bool> selfLoaded_{false};
    std::atomic<bool> peerLoaded_{false};
    std::atomic<bool> peerLeft_{false};
    std::atomic<bool> sessionSyncing_{false};

    std::unique_ptr<NetplaySession> session_;
    int layoutAtStart_ = 0;
    int framesRun_ = 0;
    int waitFrames_ = 0;   // time sync: frames still to wait
    int waitCooldown_ = 0; // time sync: frames before the next wait may start
    int desyncFrame_ = -1;
    std::string desyncReport_; // guarded by mutex_
    uint8_t predictionWindow_ = 0;
    uint8_t inputDelay_ = 0; // the local delay now (UpdatePredictionWindow)
    struct Pace {
        std::chrono::steady_clock::time_point since{};
        int frame = 0;
        int rendered = 0;
        int waits = 0;
        int focused = 0; // rendered frames with the game's window in front
        float aheadSum = 0;
    } pace_;
    NetView live_; // match figures for View(), guarded by mutex_

    // Spectating: the stream (host), and what came in (spectator).
    std::map<uint64_t, std::vector<lobby::FrameRecord>> streams_; // per match; guarded by mutex_
    std::vector<uint64_t> streamOrder_;                          // oldest first
    int streamed_ = 0;                       // game thread
    std::unique_ptr<SpectatorPlayback> spectate_;
    std::vector<lobby::Message> inbound_;    // guarded by mutex_
    std::atomic<int> specWanted_{0};
    std::atomic<uint64_t> watchingMatch_{0};

    // ----- frozen state reconciliation (lobby.h) ------------------------------
    // Written on the game thread at the freeze, before selfLoaded_; read by the
    // control thread after.
    std::vector<uint8_t> frozenState_;
    std::vector<size_t> frozenOffsets_, frozenSizes_;
    std::vector<uint64_t> frozenHashes_;
    std::atomic<bool> peerReconciled_{false}; // host: the joiner says it is done
    std::atomic<bool> reconciled_{false};     // joiner: done
    Clock::time_point bothLoadedAt_{};
    // Joiner, guarded by mutex_: the host's hashes, then the regions to fetch.
    std::vector<uint64_t> hostHashes_;
    std::vector<bool> hostHashParts_;
    bool requested_ = false;
    std::map<uint32_t, std::vector<uint8_t>> hostRegions_;      // region -> bytes
    std::map<uint32_t, std::vector<bool>> hostRegionChunks_;    // region -> chunks received
    std::atomic<bool> hostDataComplete_{false};

    void KeepFrozenState(const std::vector<StateRegion>& regions, const std::vector<uint64_t>& hashes)
    {
        frozenState_.resize(StateSize(regions));
        SaveRegions(regions, frozenState_.data());
        frozenOffsets_.clear();
        frozenSizes_.clear();
        size_t at = 0;
        for (const StateRegion& r : regions) {
            frozenOffsets_.push_back(at);
            frozenSizes_.push_back(r.size);
            at += r.size;
        }
        frozenHashes_ = hashes;
    }

    // Net thread, every 250 ms while loading.
    void SendReconcile(const std::map<uint64_t, Watcher>& watchers)
    {
        if (!options_.reconcileStart || !selfLoaded_) return;
        if (host_) {
            // Until each is done: the hashes, so it can tell what differs.
            std::vector<uint64_t> to;
            uint32_t peerChecksum;
            {
                std::lock_guard lock(mutex_);
                peerChecksum = peerChecksum_;
            }
            if (peerLoaded_ && peerChecksum != frozenChecksum_ && !peerReconciled_) to.push_back(peer_);
            for (const auto& [id, w] : watchers) {
                if (w.match == matchId_ && w.loaded && w.checksum != frozenChecksum_ && !w.reconciled && !w.streaming) {
                    to.push_back(id);
                }
            }
            for (uint64_t id : to) {
                for (size_t first = 0; first < frozenHashes_.size(); first += lobby::kHashesPerMessage) {
                    lobby::Message m;
                    m.type = lobby::Type::StateHashes;
                    m.matchId = matchId_;
                    m.first = static_cast<uint32_t>(first);
                    m.total = static_cast<uint32_t>(frozenHashes_.size());
                    const size_t n = std::min(lobby::kHashesPerMessage, frozenHashes_.size() - first);
                    m.hashes.assign(frozenHashes_.begin() + first, frozenHashes_.begin() + first + n);
                    SendTo(id, m);
                }
            }
            return;
        }
        // Joiner: ask for the regions still missing.
        lobby::Message m;
        m.type = lobby::Type::StateRequest;
        m.matchId = matchId_;
        {
            std::lock_guard lock(mutex_);
            if (!requested_ || hostDataComplete_) return;
            for (const auto& [region, chunks] : hostRegionChunks_) {
                const bool complete = !chunks.empty() && std::all_of(chunks.begin(), chunks.end(), [](bool c) { return c; });
                if (!complete && m.regions.size() < lobby::kRegionsPerRequest) m.regions.push_back(region);
            }
        }
        if (!m.regions.empty()) Send(m);
    }

    // Joiner: the host's hashes; once all are in, request every host region
    // this side has no equal of (by hash, any order).
    void OnStateHashes(const lobby::Message& m)
    {
        std::lock_guard lock(mutex_);
        if (requested_ || !selfLoaded_ || m.total == 0 || m.total > 100000) return;
        if (hostHashes_.size() != m.total) {
            hostHashes_.assign(m.total, 0);
            hostHashParts_.assign((m.total + lobby::kHashesPerMessage - 1) / lobby::kHashesPerMessage, false);
        }
        if (m.first % lobby::kHashesPerMessage || m.first + m.hashes.size() > m.total) return;
        std::copy(m.hashes.begin(), m.hashes.end(), hostHashes_.begin() + m.first);
        hostHashParts_[m.first / lobby::kHashesPerMessage] = true;
        if (!std::all_of(hostHashParts_.begin(), hostHashParts_.end(), [](bool b) { return b; })) return;
        std::multiset<uint64_t> mine(frozenHashes_.begin(), frozenHashes_.end());
        for (uint32_t i = 0; i < hostHashes_.size(); i++) {
            auto it = mine.find(hostHashes_[i]);
            if (it != mine.end()) {
                mine.erase(it);
                continue;
            }
            hostRegionChunks_[i]; // to fetch
        }
        requested_ = true;
        Log("netplay: frozen states differ in %zu of %zu regions; taking the host's", hostRegionChunks_.size(),
            hostHashes_.size());
        if (hostRegionChunks_.empty()) hostDataComplete_ = true;
    }

    // Host: the bytes of the requested regions, in chunks.
    void OnStateRequest(uint64_t to, const lobby::Message& m)
    {
        for (uint32_t region : m.regions) {
            if (region >= frozenSizes_.size()) continue;
            const size_t size = frozenSizes_[region];
            for (size_t at = 0; at < size; at += lobby::kStateChunk) {
                lobby::Message d;
                d.type = lobby::Type::StateData;
                d.matchId = matchId_;
                d.region = region;
                d.total = static_cast<uint32_t>(size);
                d.offset = static_cast<uint32_t>(at);
                const size_t n = std::min(lobby::kStateChunk, size - at);
                const uint8_t* src = frozenState_.data() + frozenOffsets_[region] + at;
                d.bytes.assign(src, src + n);
                SendTo(to, d);
            }
        }
    }

    // Joiner: a chunk of a host region.
    void OnStateData(const lobby::Message& m)
    {
        std::lock_guard lock(mutex_);
        auto it = hostRegionChunks_.find(m.region);
        if (it == hostRegionChunks_.end() || m.total == 0 || m.total > (1u << 24)) return;
        auto& bytes = hostRegions_[m.region];
        if (bytes.size() != m.total) {
            bytes.assign(m.total, 0);
            it->second.assign((m.total + lobby::kStateChunk - 1) / lobby::kStateChunk, false);
        }
        if (m.offset % lobby::kStateChunk || m.offset + m.bytes.size() > m.total) return;
        std::copy(m.bytes.begin(), m.bytes.end(), bytes.begin() + m.offset);
        it->second[m.offset / lobby::kStateChunk] = true;
        for (const auto& [region, chunks] : hostRegionChunks_) {
            if (chunks.empty() || !std::all_of(chunks.begin(), chunks.end(), [](bool c) { return c; })) return;
        }
        hostDataComplete_ = true;
    }

    // Joiner, game thread: copies the host's values into this side's regions.
    // Each host region pairs with the most similar region of the same size
    // that has no equal on the host (same-size regions are ordered by
    // content, which can differ). Masked words and pointer-like values stay.
    void ApplyHostState(SfvGame& game)
    {
        const auto& regions = game.Regions();
        std::map<uint32_t, std::vector<uint8_t>> host;
        std::multiset<uint64_t> hostHashes;
        {
            std::lock_guard lock(mutex_);
            host = hostRegions_;
            hostHashes.insert(hostHashes_.begin(), hostHashes_.end());
        }
        std::vector<bool> taken(regions.size(), false);
        for (size_t i = 0; i < regions.size(); i++) {
            // Regions with an equal on the host are not candidates.
            auto it = hostHashes.find(frozenHashes_[i]);
            if (it != hostHashes.end()) {
                hostHashes.erase(it);
                taken[i] = true;
            }
        }
        size_t regionsChanged = 0, wordsCopied = 0, pointersKept = 0;
        for (const auto& [hostIndex, bytes] : host) {
            size_t best = SIZE_MAX, bestDiff = SIZE_MAX;
            for (size_t i = 0; i < regions.size(); i++) {
                if (taken[i] || regions[i].size != bytes.size()) continue;
                size_t diff = 0;
                for (size_t w = 0; w + 8 <= bytes.size(); w += 8) diff += std::memcmp(regions[i].ptr + w, &bytes[w], 8) != 0;
                if (diff < bestDiff) {
                    best = i;
                    bestDiff = diff;
                }
            }
            if (best == SIZE_MAX) {
                Log("netplay: no region here pairs with the host's region %u (%zu bytes)", hostIndex, bytes.size());
                continue;
            }
            taken[best] = true;
            const StateRegion& r = regions[best];
            size_t copied = 0;
            for (size_t w = 0; w + 8 <= bytes.size(); w += 8) {
                const size_t word = w / 8;
                if (word / 8 < r.ignore.size() && (r.ignore[word / 8] >> (word % 8) & 1)) continue;
                uint64_t mine, theirs;
                std::memcpy(&mine, r.ptr + w, 8);
                std::memcpy(&theirs, &bytes[w], 8);
                if (mine == theirs) continue;
                if (PointerLike(mine) || PointerLike(theirs)) {
                    pointersKept++;
                    continue;
                }
                std::memcpy(r.ptr + w, &theirs, 8);
                copied++;
            }
            if (copied) {
                regionsChanged++;
                wordsCopied += copied;
                Log("netplay:   %s (%zu bytes): %zu words from the host", r.name.c_str(), r.size, copied);
            }
        }
        std::vector<uint64_t> hashes(regions.size());
        HashRegions(regions, hashes.data());
        frozenChecksum_ = FoldChecksum(hashes.data(), hashes.size());
        frozenHashes_ = hashes;
        Log("netplay: took the host's values: %zu words in %zu regions (%zu pointer-like words kept); checksum now %08X",
            wordsCopied, regionsChanged, pointersKept, frozenChecksum_);
    }

    // Both loaded: start when the frozen states agree, when the joiner has
    // taken the host's values, or after kReconcileSeconds.
    bool ReadyToStart(SfvGame& game, uint32_t peerChecksum)
    {
        if (bothLoadedAt_ == Clock::time_point{}) bothLoadedAt_ = Clock::now();
        const bool late = Clock::now() - bothLoadedAt_ > std::chrono::seconds(kReconcileSeconds);
        if (frozenChecksum_ == peerChecksum && (host_ || !requested_ || reconciled_)) return true;
        if (!options_.reconcileStart) return true;
        if (host_) {
            if (peerReconciled_) return true;
        }
        else if (hostDataComplete_ && !reconciled_) {
            ApplyHostState(game);
            reconciled_ = true;
            return true;
        }
        else if (reconciled_) {
            return true;
        }
        if (late) {
            Log("netplay: the frozen states could not be reconciled in %d s; starting anyway", kReconcileSeconds);
            reconciled_ = true;
            return true;
        }
        return false;
    }
};

SfvGame* g_game = nullptr;
NetplayOptions g_options;
bool g_keyDown[2] = {};
std::wstring g_reportDir;
std::string g_modVersion;
std::unique_ptr<NetplayDriver> g_driver;
std::mutex g_driverMutex;

void Replace(std::unique_ptr<NetplayDriver> next)
{
    std::lock_guard lock(g_driverMutex);
    if (g_driver) {
        // Detach, then give the game thread a frame to leave OnFrame.
        if (CurrentFrameDriver() == g_driver.get()) SetFrameDriver(nullptr);
        Sleep(100);
        g_driver->Stop();
    }
    g_driver = std::move(next);
}

} // namespace

void InstallNetplay(SfvGame* game, const std::wstring& reportDir, const std::string& modVersion,
                    const NetplayOptions& defaults)
{
    g_options = defaults;
    g_game = game;
    g_reportDir = reportDir;
    g_modVersion = modVersion;
    // Keep the picks loaded while waiting at the menu: this player's, and in a
    // session the other's. Nothing while a match runs.
    SetPreloadSource([] {
        std::vector<PreloadPick> picks;
        {
            std::unique_lock lock(g_driverMutex, std::try_to_lock);
            if (!lock.owns_lock()) return picks;
            if (g_driver) return g_driver->PreloadPicks();
        }
        const NetplayOptions& o = g_options;
        if (o.hasPick && o.pick.character && !o.randomSetup) {
            picks.push_back({UnpackCode(o.pick.character), static_cast<int>(o.pick.costume)});
        }
        return picks;
    });
}

void NetHost(const NetplayOptions& options)
{
    if (!g_game) return;
    // The previous session (also one that ended) still holds its socket.
    Replace(nullptr);
    auto d = std::make_unique<NetplayDriver>(*g_game, g_reportDir, g_modVersion);
    std::string error;
    if (!d->Open(options, true, lobby::Role::Player, &error)) {
        Log("netplay: cannot host: %s", error.c_str());
        SetStatus("cannot host: " + error);
        SetResult("cannot host: " + error);
        return;
    }
    d->StartHost();
    Replace(std::move(d));
}

void NetJoin(const std::string& code, const NetplayOptions& options, bool spectate)
{
    if (!g_game) return;
    Replace(nullptr);
    auto d = std::make_unique<NetplayDriver>(*g_game, g_reportDir, g_modVersion);
    std::string error;
    if (!d->Open(options, false, spectate ? lobby::Role::Spectator : lobby::Role::Player, &error)) {
        Log("netplay: cannot join: %s", error.c_str());
        SetStatus("cannot join: " + error);
        SetResult("cannot join: " + error);
        return;
    }
    if (!d->StartJoin(code)) {
        Log("netplay: not a valid match code: %s", code.c_str());
        SetStatus("not a valid match code");
        SetResult("not a valid match code");
        return;
    }
    Replace(std::move(d));
}

void NetLeave()
{
    Replace(nullptr);
    if (g_options.menuAfterMatch && g_game && g_game->BattleRunning()) ReturnToMainMenu(1);
    SetStatus("");
    Log("netplay: left");
}

void NetSetReady(bool ready)
{
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetReady(ready);
}

void NetSetPick(bool hasPick, const PlayerSetup& pick)
{
    g_options.hasPick = hasPick;
    g_options.pick = pick;
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetPick(hasPick, pick);
}

void NetSetDelay(uint8_t frames)
{
    g_options.inputDelay = frames;
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetDelay(frames);
}

void NetSetStage(const std::string& code)
{
    g_options.stageCode = code;
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetStage(code);
}

void NetChoose(lobby::Choice choice)
{
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->Choose(choice);
}

void NetPark()
{
    if (!g_game || !g_game->BattleTicking()) {
        Log("park: no battle running");
        return;
    }
    std::thread([] {
        ThreadGuard running;
        static const std::atomic<bool> never{false};
        Log("park: %s", LeaveBattleForStageSelect(*g_game, never) ? "at stage select" : "could not pause the battle");
        SetScriptedPad(0, false);
    }).detach();
}

void NetSetConditions(const NetConditions& conditions)
{
    std::lock_guard lock(g_driverMutex);
    if (g_driver) g_driver->SetConditions(conditions);
}

NetplayOptions& NetOptions() { return g_options; }

std::string BattleInfo()
{
    if (!g_game) return "no game";
    std::string s = g_game->BattleRunning() ? "running" : "not running";
    s += g_game->BattleTicking() ? ", ticking" : ", not ticking";
    auto words = [&](const char* name, const uint8_t* p, size_t size) {
        if (!p) return;
        s += std::string(" | ") + name;
        char buf[16];
        for (size_t i = 0; i < size; i += 4) {
            uint32_t v;
            std::memcpy(&v, p + i, 4);
            std::snprintf(buf, sizeof(buf), " %x", v);
            s += buf;
        }
    };
    if (uint8_t* bf = g_game->BattleFrameObject()) {
        char at[48];
        std::snprintf(at, sizeof(at), " | battleframe at %p:", static_cast<void*>(bf));
        s += at;
        words("", bf + game::battleframe::kStateBegin, game::battleframe::kStateSize);
        words("extra", bf + game::battleframe::kExtraTicks, 4);
    }
    if (uint8_t* iu = g_game->InputUnit()) words("inputunit", iu + game::inputunit::kCountersBegin, game::inputunit::kCountersSize);
    s += " | regions " + std::to_string(g_game->Regions().size());
    if (uint8_t* data = BattleData()) {
        char at[48];
        std::snprintf(at, sizeof(at), " | battledata at %p:", static_cast<void*>(data));
        s += at;
        words("", data, battledata::kCharaCode);
    }
    return s;
}

void NetPollHotkeys()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(RealForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    const int keys[2] = {VK_F7, VK_F8};
    for (int i = 0; i < 2; i++) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool pressed = down && !g_keyDown[i];
        g_keyDown[i] = down;
        if (!pressed) continue;
        if (i == 0) {
            NetHost(g_options);
            continue;
        }
        std::string code;
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_TEXT)) {
                if (const char* text = static_cast<const char*>(GlobalLock(h))) {
                    code = text;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        NetJoin(code, g_options);
    }
}

std::string NetStatus()
{
    std::lock_guard lock(g_driverMutex);
    return g_driver ? g_driver->Status() : "idle";
}

NetView NetViewNow()
{
    static std::mutex lastMutex;
    static NetView last;
    std::unique_lock lock(g_driverMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::lock_guard l(lastMutex);
        return last;
    }
    NetView v = g_driver ? g_driver->View() : NetView{};
    lock.unlock();
    v.result = LastResult();
    std::lock_guard l(lastMutex);
    last = v;
    return v;
}

} // namespace sfv::mod
