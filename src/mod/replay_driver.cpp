#include "mod/replay_driver.h"

#include "core/netplay.h"
#include "core/replay_file.h"
#include "core/state.h"
#include "core/transport.h"
#include "mod/battle_driver.h"
#include "mod/battle_setup.h"
#include "mod/control.h"
#include "mod/files.h"
#include "mod/flow.h"
#include "mod/log.h"
#include "mod/sfv_game.h"
#include "mod/ue4.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

std::atomic<bool> g_stop{false}; // `replay stop`: end the running replay at the next frame

class NoTransport : public ITransport {
public:
    void Send(uint64_t, const uint8_t*, size_t) override {}
    bool Receive(Packet&) override { return false; }
};

// Where a bare file name is looked for: SFVR\matches (files.h), then the
// reports folder earlier builds wrote to (next to the exe).
std::string FindRecording(const std::string& name)
{
    std::string matches;
    for (wchar_t c : MatchesDir()) matches += c < 128 ? static_cast<char>(c) : '_';
    const std::string inMatches = matches + "\\" + name;
    if (GetFileAttributesA(inMatches.c_str()) != INVALID_FILE_ATTRIBUTES) return inMatches;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::string out;
    for (wchar_t c : dir) out += c < 128 ? static_cast<char>(c) : '_';
    return out + "\\reports\\" + name;
}

class ReplayDriver : public IFrameDriver {
public:
    ReplayDriver(ReplayFile rf, std::string name, int stress, int watchFrom, int watchTo, std::string watchRegion)
        : rf_(std::move(rf)), name_(std::move(name)), stress_(stress), creationsAtJump_(BattleCreationCount()),
          watchFrom_(watchFrom), watchTo_(watchTo), watchRegion_(std::move(watchRegion))
    {
        last_ = rf_.finalThrough >= 0 ? std::min(rf_.finalThrough, rf_.FrameCount() - 1) : rf_.FrameCount() - 1;
    }

    bool OnFrame(SfvGame& game) override
    {
        if (g_stop.exchange(false)) {
            Log("replay: stopped");
            Finish(game);
            return false;
        }
        // The battle we jumped from runs until the new one is created.
        if (!jumped_) {
            if (BattleCreationCount() == creationsAtJump_) return false;
            jumped_ = true;
            game.ForgetBattleObjects();
            Log("replay: battle created (%s)", RoundEndState().c_str());
        }
        if (!ready_) return Warmup(game);
        if (stress_ > 0) return StressFrame(game);
        return StraightFrame(game);
    }

private:
    // Neutral ticks until the battle's objects exist, then frozen at frame 0
    // as a netplay session is.
    bool Warmup(SfvGame& game)
    {
        game.RefreshRegions();
        if (game.Regions().empty() || !game.InputUnit()) {
            game.Advance(FrameInputs{}, false);
            if (++warmupTicks_ % 300 == 0) Log("replay: still no battle objects after %d ticks", warmupTicks_);
            return true;
        }
        ready_ = true;
        SetPadIsolation(true);
        game.exclusiveTicks = true;
        SetRoundDirectorPerTick(true, true);
        SetMatchDrawSeed(rf_.setup.seed);
        game.Recanonicalize();
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(game.Regions(), hashes.data());
        // The neutral ticks before the objects exist depend on loading (16 in
        // a session, 9 here), and so does the frozen state: take the
        // recording's, as a netplay joiner takes the host's.
        if (Compare(game, hashes.data(), rf_.initialHashes.data(), -2)) {
            TakeRecordedStart(game, hashes);
            game.Recanonicalize();
            hashes.assign(game.Regions().size(), 0);
            HashRegions(game.Regions(), hashes.data());
        }
        layout_ = game.LayoutVersion(); // after the copy and its new canonical order
        const int differ = Compare(game, hashes.data(), rf_.initialHashes.data(), -1);
        Log("replay: battle objects ready after %d neutral ticks (%s); state before frame 0 %s", warmupTicks_,
            RoundEndState().c_str(), differ ? ("differs in " + std::to_string(differ) + " regions").c_str() : "equal");
        // Replays sent soon after the game started had the objects after 3
        // ticks, and every one of them went on differently from frame 0 (its
        // camera did not follow the fight); 6 or more always matched, and
        // sessions took 6 to 18. Not understood yet; more ticks once the
        // objects exist do not help (the battle moves on from the recording).
        if (warmupTicks_ < 6) {
            Log("replay: WARNING: the battle was ready after only %d ticks; such replays have differed from their "
                "recording without a real divergence: run it again",
                warmupTicks_);
        }
        if (stress_ > 0) {
            NetplayConfig nc;
            nc.stressCheckDistance = static_cast<unsigned char>(stress_);
            nc.predictionWindow = static_cast<unsigned char>(stress_ + 1);
            nc.historyDepth = static_cast<unsigned int>(stress_ + 2);
            nc.inputDelay = 0;
            nc.modVersion = "replay";
            nc.log = [](const std::string& m) { Log("replay: %s", m.c_str()); };
            // Both players' inputs of the next frame from the recording.
            game.inputOverride = [this](int player) {
                const int f = std::min(fed_, rf_.FrameCount() - 1);
                const PackedInput in = rf_.inputs[f].p[player & 1];
                if (player == 1) fed_++;
                return in;
            };
            session_ = std::make_unique<NetplaySession>(game, transport_, nc);
            // Resimulated words, by region; the presentation clocks apart.
            session_->onStressDiff = [this, &game](int frame, size_t region, size_t offset) {
                if (region >= game.Regions().size()) return;
                const std::string& name = game.Regions()[region].name;
                if (name == "viewtarget.clock" || name == "pawn.clock") return;
                if (stressFrames_.insert(frame).second) {
                    stressRegionFrames_[name]++;
                    if (stressLogged_++ < 20) {
                        Log("replay: frame %d resimulated differently in %s (+0x%zx)", frame, name.c_str(), offset);
                    }
                }
            };
            std::string error;
            if (!session_->Start(&error)) {
                Log("replay: cannot start the stress session: %s", error.c_str());
                Finish(game);
                return false;
            }
        }
        return true;
    }

    // Copies the recording's state before frame 0 (its dump) into the regions
    // that differ, as ApplyHostState in netplay_driver.cpp does: each recorded
    // region without an equal here pairs with the most similar region of the
    // same size; masked words and pointer-like values stay.
    void TakeRecordedStart(SfvGame& game, const std::vector<uint64_t>& hashes)
    {
        const StateDump* dump = rf_.FindDump(-1);
        if (!dump) {
            Log("replay: the recording has no state before frame 0 to start from");
            return;
        }
        const auto& regions = game.Regions();
        std::multiset<uint64_t> mine(hashes.begin(), hashes.end());
        std::multiset<uint64_t> recorded(rf_.initialHashes.begin(), rf_.initialHashes.end());
        std::vector<bool> taken(regions.size(), false);
        for (size_t i = 0; i < regions.size(); i++) {
            auto it = recorded.find(hashes[i]);
            if (it == recorded.end()) continue;
            recorded.erase(it);
            taken[i] = true;
        }
        size_t at = 0, changed = 0, words = 0, pointers = 0;
        for (size_t j = 0; j < rf_.regions.size(); j++) {
            const size_t size = static_cast<size_t>(rf_.regions[j].size);
            const uint8_t* bytes = dump->bytes.data() + at;
            at += size;
            if (at > dump->bytes.size()) break;
            auto it = mine.find(rf_.initialHashes[j]);
            if (it != mine.end()) {
                mine.erase(it);
                continue;
            }
            size_t best = SIZE_MAX, bestDiff = SIZE_MAX;
            for (size_t i = 0; i < regions.size(); i++) {
                if (taken[i] || regions[i].size != size) continue;
                size_t diff = 0;
                for (size_t w = 0; w + 8 <= size; w += 8) diff += std::memcmp(regions[i].ptr + w, bytes + w, 8) != 0;
                if (diff < bestDiff) {
                    best = i;
                    bestDiff = diff;
                }
            }
            if (best == SIZE_MAX) continue;
            taken[best] = true;
            const StateRegion& r = regions[best];
            size_t copied = 0;
            for (size_t w = 0; w + 8 <= size; w += 8) {
                const size_t word = w / 8;
                if (word / 8 < r.ignore.size() && (r.ignore[word / 8] >> (word % 8) & 1)) continue;
                uint64_t here, there;
                std::memcpy(&here, r.ptr + w, 8);
                std::memcpy(&there, bytes + w, 8);
                if (here == there) continue;
                const auto pointerLike = [](uint64_t v) { return v >= 0x10000000000ull && v < 0x800000000000ull; };
                if (pointerLike(here) || pointerLike(there)) {
                    if (pointers++ < 8) {
                        Log("replay:   %s +0x%zx kept: %016llx here, %016llx recorded", r.name.c_str(), w,
                            static_cast<unsigned long long>(here), static_cast<unsigned long long>(there));
                    }
                    continue;
                }
                std::memcpy(r.ptr + w, &there, 8);
                copied++;
            }
            if (copied) {
                changed++;
                words += copied;
                Log("replay:   %s (%zu bytes): %zu words from the recording", r.name.c_str(), r.size, copied);
            }
        }
        Log("replay: took the recording's start: %zu words in %zu regions (%zu pointer-like words kept)", words, changed,
            pointers);
    }

    bool StraightFrame(SfvGame& game)
    {
        if (!Valid(game)) return false;
        if (frame_ > last_) {
            Finish(game);
            return false;
        }
        game.Advance(rf_.inputs[frame_], false);
        CheckFrame(game, frame_);
        WatchFrame(game, frame_);
        frame_++;
        return true;
    }

    bool StressFrame(SfvGame& game)
    {
        if (!Valid(game)) return false;
        if (session_->NextFrame() > last_) {
            Finish(game);
            return false;
        }
        session_->Update();
        // The state now is the end of the newest frame (after its
        // resimulation): it must equal the recording too.
        const int f = session_->NextFrame() - 1;
        WatchFrame(game, f);
        if (f >= 0 && f != checked_) {
            CheckFrame(game, f);
            checked_ = f;
        }
        return true;
    }

    // `watch <from> <to> [region]`: per frame, the region's first 16 bytes
    // (by name) and the round's end state.
    void WatchFrame(SfvGame& game, int f)
    {
        if (f < watchFrom_ || f > watchTo_) return;
        std::string bytes = "-";
        for (const StateRegion& r : game.Regions()) {
            if (r.name != watchRegion_) continue;
            char b[64];
            uint64_t w[2] = {};
            std::memcpy(w, r.ptr, std::min<size_t>(16, r.size));
            std::snprintf(b, sizeof(b), "%016llx %016llx", static_cast<unsigned long long>(w[0]),
                          static_cast<unsigned long long>(w[1]));
            bytes = b;
        }
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(game.Regions(), hashes.data());
        Log("replay: watch frame %d: %s %s | %s | state %08x | camera %s", f, watchRegion_.c_str(), bytes.c_str(),
            RoundEndState().c_str(), FoldChecksum(hashes.data(), hashes.size()), CameraText().c_str());
    }

    // The battle camera as last drawn: KWBattleCameraManager's CameraCache.POV
    // (+0x368 + 0x8): location, rotation (pitch, yaw, roll), FOV. Outside the
    // snapshot; compared between straight and stress replays by hand.
    std::string CameraText()
    {
        constexpr size_t kPov = 0x370;
        // The one drawn: the PlayerCameraManager (+0x390) of the player
        // controller that has a local player (Player, +0x360); the other
        // managers follow pawns. A walk over every object is slow: one try
        // per 120 frames until found.
        constexpr size_t kPlayer = 0x360, kCameraManager = 0x390;
        if (!camera_ && cameraTries_++ % 120 == 0 && ue4::EnsureObjects(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) {
            ue4::ForEachObject([&](const void* object) {
                if (camera_ || ue4::ObjectName(ue4::ClassOf(object)).find("PlayerController") == std::string::npos) return;
                const auto* p = static_cast<const uint8_t*>(object);
                if (!IsReadable(p + kCameraManager, 8) || !*reinterpret_cast<const void* const*>(p + kPlayer)) return;
                const auto* manager = *reinterpret_cast<const uint8_t* const*>(p + kCameraManager);
                if (manager && ue4::ObjectName(ue4::ClassOf(manager)).find("CameraManager") != std::string::npos) camera_ = manager;
            });
            if (camera_) Log("replay: camera manager %s (%p)", ue4::Describe(camera_).c_str(), static_cast<const void*>(camera_));
        }
        if (!camera_ || !IsReadable(camera_ + kPov, 0x1C)) return "-";
        float v[7];
        std::memcpy(v, camera_ + kPov, sizeof(v));
        char b[128];
        std::snprintf(b, sizeof(b), "%.1f %.1f %.1f r %.2f %.2f %.2f fov %.2f", v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
        return b;
    }

    bool Valid(SfvGame& game)
    {
        game.RefreshRegions();
        if (game.LayoutVersion() == layout_ && !game.Regions().empty()) return true;
        Log("replay: the battle ended or changed under the replay at frame %d", stress_ > 0 ? session_->NextFrame() : frame_);
        Finish(game);
        return false;
    }

    void CheckFrame(SfvGame& game, int f)
    {
        if (f >= rf_.FrameCount() || rf_.hashes.empty()) return;
        std::vector<uint64_t> hashes(game.Regions().size());
        HashRegions(game.Regions(), hashes.data());
        framesChecked_++;
        if (rf_.HasRegionHashes(f)) {
            Compare(game, hashes.data(), rf_.FrameHashes(f), f);
            return;
        }
        // A compact recording: the frame's checksum only (a mismatch names no
        // region), and every 600 frames a state dump, compared word by word.
        const bool equal = FoldChecksum(hashes.data(), hashes.size()) == rf_.FrameChecksum(f);
        if (const StateDump* dump = rf_.FindDump(f)) CompareDump(game, *dump, f);
        if (equal) return;
        if (firstMismatch_ < 0) firstMismatch_ = f;
        mismatchFrames_++;
        if (logged_++ < 12) Log("replay: frame %d's checksum differs from the recording (no region hashes kept)", f);
    }

    // The state against the recording's dump of the same frame: each region
    // here pairs with the same-size dump region it differs least from
    // (masked words left out); the differing words are logged.
    void CompareDump(SfvGame& game, const StateDump& dump, int f)
    {
        struct Recorded {
            const uint8_t* bytes;
            size_t size;
            bool taken;
        };
        std::vector<Recorded> recorded;
        size_t at = 0;
        for (const auto& r : rf_.regions) {
            const size_t size = static_cast<size_t>(r.size);
            if (at + size > dump.bytes.size()) break;
            recorded.push_back({dump.bytes.data() + at, size, false});
            at += size;
        }
        int differing = 0, shown = 0;
        for (const StateRegion& r : game.Regions()) {
            auto masked = [&r](size_t word) { return word / 8 < r.ignore.size() && (r.ignore[word / 8] >> (word % 8) & 1); };
            auto diff = [&](const Recorded& d) {
                size_t n = 0;
                for (size_t w = 0; w + 8 <= r.size; w += 8) n += !masked(w / 8) && std::memcmp(r.ptr + w, d.bytes + w, 8) != 0;
                return n;
            };
            Recorded* best = nullptr;
            size_t bestDiff = SIZE_MAX;
            for (auto& d : recorded) {
                if (d.taken || d.size != r.size) continue;
                const size_t n = diff(d);
                if (n < bestDiff) {
                    best = &d;
                    bestDiff = n;
                    if (n == 0) break;
                }
            }
            if (!best) continue;
            best->taken = true;
            if (bestDiff == 0) continue;
            differing++;
            for (size_t w = 0; w + 8 <= r.size && shown < 24; w += 8) {
                if (masked(w / 8) || std::memcmp(r.ptr + w, best->bytes + w, 8) == 0) continue;
                uint64_t mine, theirs;
                std::memcpy(&mine, r.ptr + w, 8);
                std::memcpy(&theirs, best->bytes + w, 8);
                Log("replay: frame %d dump: %s +0x%zx here %016llx, recorded %016llx", f, r.name.c_str(), w,
                    static_cast<unsigned long long>(mine), static_cast<unsigned long long>(theirs));
                shown++;
            }
        }
        Log("replay: frame %d against the recording's dump: %d regions differ", f, differing);
    }

    // Region by region, by content: each region here must have an equal
    // among the recording's regions of its size. (Names do not pair them: the
    // canonical order numbers same-size regions by content, and the battle
    // can bring a region more or less, which shifts every name after it.)
    int Compare(SfvGame& game, const uint64_t* ours, const uint64_t* recorded, int f)
    {
        if (!recorded) return 0;
        std::map<uint64_t, std::multiset<uint64_t>> bySize;
        for (size_t j = 0; j < rf_.regions.size(); j++) bySize[rf_.regions[j].size].insert(recorded[j]);
        int differ = 0;
        std::string names;
        for (size_t i = 0; i < game.Regions().size(); i++) {
            auto& pool = bySize[game.Regions()[i].size];
            const auto it = pool.find(ours[i]);
            if (it != pool.end()) {
                pool.erase(it);
                continue;
            }
            // The camera's and the pawns' animation clocks follow how ticks
            // fall into rendered frames: presentation, counted apart.
            const std::string& name = game.Regions()[i].name;
            if (f >= 0 && (name == "viewtarget.clock" || name == "pawn.clock")) {
                presentationFrames_++;
                continue;
            }
            differ++;
            if (f >= 0) regionFrames_[name]++;
            if (names.size() < 160) names += (names.empty() ? "" : ", ") + name;
        }
        if (differ && f >= 0) {
            if (firstMismatch_ < 0) firstMismatch_ = f;
            mismatchFrames_++;
            if (logged_++ < 12) Log("replay: frame %d differs from the recording in %d regions: %s", f, differ, names.c_str());
        }
        if (differ && f == -1) Log("replay: state before frame 0 differs in %d regions: %s", differ, names.c_str());
        return differ;
    }

    void Finish(SfvGame& game)
    {
        game.inputOverride = nullptr;
        game.exclusiveTicks = false;
        SetRoundDirectorPerTick(false, false);
        SetPadIsolation(false);
        // Stress: frames whose resimulation differed outside the presentation clocks.
        const int stressDesyncs = static_cast<int>(stressFrames_.size());
        const bool pass = ready_ && firstMismatch_ < 0 && stressDesyncs == 0 && framesChecked_ > last_;
        Log("replay: %s %s, %d of %d frames checked%s, %d frames differ from the recording (first %d), "
            "%d presentation clock differences%s", name_.c_str(),
            pass ? "PASS" : "FAIL", framesChecked_, last_ + 1, stress_ > 0 ? (", stress distance " + std::to_string(stress_)).c_str() : "",
            mismatchFrames_, firstMismatch_, presentationFrames_,
            stress_ > 0 ? (", " + std::to_string(stressDesyncs) + " frames resimulated differently").c_str() : "");
        // Which regions, on how many frames: one or two small regions on many
        // frames point at a presentation or loading-timing word, many regions
        // at a real divergence.
        std::vector<std::pair<int, std::string>> byCount;
        for (const auto& [name, frames] : regionFrames_) byCount.push_back({frames, name});
        std::sort(byCount.rbegin(), byCount.rend());
        std::string top;
        for (size_t i = 0; i < byCount.size() && i < 12; i++) {
            top += (top.empty() ? "" : ", ") + byCount[i].second + " " + std::to_string(byCount[i].first);
        }
        if (!top.empty()) Log("replay:   regions (frames): %s", top.c_str());
        top.clear();
        byCount.clear();
        for (const auto& [name, frames] : stressRegionFrames_) byCount.push_back({frames, name});
        std::sort(byCount.rbegin(), byCount.rend());
        for (size_t i = 0; i < byCount.size() && i < 12; i++) {
            top += (top.empty() ? "" : ", ") + byCount[i].second + " " + std::to_string(byCount[i].first);
        }
        if (!top.empty()) Log("replay:   resimulated differently (frames): %s", top.c_str());
        SetFrameDriver(nullptr);
        delete this;
    }

    ReplayFile rf_;
    std::string name_;
    int stress_;
    int creationsAtJump_;
    int watchFrom_, watchTo_;
    std::string watchRegion_;
    const uint8_t* camera_ = nullptr; // the battle's camera manager (CameraText)
    int cameraTries_ = 0;
    int last_ = -1;
    bool jumped_ = false;
    bool ready_ = false;
    int warmupTicks_ = 0;
    int layout_ = 0;
    int frame_ = 0;
    int fed_ = 0;
    int checked_ = -1;
    int framesChecked_ = 0;
    int firstMismatch_ = -1;
    int mismatchFrames_ = 0;
    int presentationFrames_ = 0; // camera / pawn clock differences (not a failure)
    std::map<std::string, int> regionFrames_; // frames each region differed on
    std::set<int> stressFrames_;                    // frames resimulated differently (not the clocks)
    std::map<std::string, int> stressRegionFrames_;
    int stressLogged_ = 0;
    int logged_ = 0;
    NoTransport transport_;
    std::unique_ptr<NetplaySession> session_;
};

} // namespace

void RequestReplay(const std::string& path, int stressDistance, int watchFrom, int watchTo, const std::string& watchRegion)
{
    if (path == "stop") {
        g_stop = CurrentFrameDriver() != nullptr;
        return;
    }
    if (CurrentFrameDriver()) {
        Log("replay: another driver is active (leave the session first)");
        return;
    }
    const bool relative = path.size() < 2 || (path[1] != ':' && path[0] != '\\' && path[0] != '/');
    const std::string full = relative ? FindRecording(path) : path;
    ReplayFile rf;
    std::string error;
    if (!LoadReplay(full, &rf, &error)) {
        Log("replay: %s: %s", full.c_str(), error.c_str());
        return;
    }
    stressDistance = std::clamp(stressDistance, 0, 30);
    const std::string name = full.substr(full.find_last_of("\\/") + 1);
    Log("replay: %s, %d frames (confirmed through %d), build %s, %s%s", name.c_str(), rf.FrameCount(), rf.finalThrough,
        rf.gameBuild.c_str(), DescribeSetup(rf.setup).c_str(),
        stressDistance ? (", stress distance " + std::to_string(stressDistance)).c_str() : "");
    // Both characters loaded first, as a netplay lobby has them (assets that
    // load while the battle runs can put effects out of step, finding 23).
    // The jump follows.
    std::vector<PreloadPick> picks;
    for (const PlayerSetup& p : rf.setup.players) {
        if (p.character) picks.push_back({UnpackCode(p.character), static_cast<int>(p.costume)});
    }
    // The recorded setup for the battle the jump starts.
    SetPendingSetup(rf.setup);
    SetFrameDriver(new ReplayDriver(std::move(rf), name, stressDistance, watchFrom, watchTo, watchRegion));
    PreloadCharacters(std::move(picks), [] { JumpToBattle(); });
}

} // namespace sfv::mod
