#include "mod/replay_driver.h"

#include "core/netplay.h"
#include "core/replay_file.h"
#include "core/state.h"
#include "core/transport.h"
#include "mod/battle_driver.h"
#include "mod/battle_setup.h"
#include "mod/control.h"
#include "mod/flow.h"
#include "mod/log.h"
#include "mod/sfv_game.h"

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

// The game's reports folder (next to its exe).
std::string ReportsDir()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::string out;
    for (wchar_t c : dir) out += c < 128 ? static_cast<char>(c) : '_';
    return out + "\\reports\\";
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
            Log("replay: battle created");
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
        Log("replay: battle objects ready after %d neutral ticks; state before frame 0 %s", warmupTicks_,
            differ ? ("differs in " + std::to_string(differ) + " regions").c_str() : "equal");
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
        Log("replay: watch frame %d: %s %s | %s", f, watchRegion_.c_str(), bytes.c_str(), RoundEndState().c_str());
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
        Compare(game, hashes.data(), rf_.FrameHashes(f), f);
        framesChecked_++;
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
    const std::string full = relative ? ReportsDir() + path : path;
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
    // The recorded setup for the battle the jump starts.
    SetPendingSetup(rf.setup);
    SetFrameDriver(new ReplayDriver(std::move(rf), name, stressDistance, watchFrom, watchTo, watchRegion));
    JumpToBattle();
}

} // namespace sfv::mod
