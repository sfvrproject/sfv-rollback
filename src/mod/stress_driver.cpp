#include "mod/stress_driver.h"

#include "core/netplay.h"
#include "core/state.h"
#include "core/transport.h"
#include "mod/control.h"
#include "mod/flow.h"
#include "mod/log.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <vector>

namespace sfv::mod {

namespace {

class NoTransport : public ITransport {
public:
    void Send(uint64_t, const uint8_t*, size_t) override {}
    bool Receive(Packet&) override { return false; }
};

std::wstring g_noisePath;

class StressDriver : public IFrameDriver {
public:
    StressDriver(int frames, int distance, uint64_t seed, bool learn)
        : frames_(frames), distance_(distance), seed_(seed), learn_(learn)
    {
    }

    bool OnFrame(SfvGame& game) override
    {
        if (!session_) {
            NetplayConfig nc;
            nc.stressCheckDistance = static_cast<unsigned char>(distance_);
            nc.predictionWindow = static_cast<unsigned char>(distance_ + 1);
            nc.modVersion = "stress";
            nc.historyDepth = static_cast<unsigned int>(distance_ + 2);
            nc.log = [](const std::string& m) { Log("stress: %s", m.c_str()); };
            game.bothPadsLocal = true;
            session_ = std::make_unique<NetplaySession>(game, transport_, nc);
            session_->onStressDiff = [this, &game](int frame, size_t region, size_t offset) {
                diffWords_++;
                pending_.push_back({frame, region, offset});
                // Which words, for the first few (a learning run logs them as it learns).
                if (!learn_ && diffWords_ <= 40 && region < game.Regions().size()) {
                    Log("stress:   frame %d %s +0x%zx differs", frame, game.Regions()[region].name.c_str(), offset);
                }
            };
            std::string error;
            if (!session_->Start(&error)) {
                Log("stress: cannot start: %s", error.c_str());
                Finish(game);
                return false;
            }
            start_ = std::chrono::steady_clock::now();
            layout_ = game.LayoutVersion();
            // As in a netplay session: the round director per tick, and the
            // round's end rolled back (flow.h); every frame is replayed
            // `distance` deep, so every round's end is crossed.
            SetRoundDirectorPerTick(true, true);
        }

        // The battle ended or changed under the test (a KO, a menu): stop.
        game.RefreshRegions();
        if (game.LayoutVersion() != layout_ || game.Regions().empty()) {
            Log("stress: the battle ended after %d frames; stopping", frame_);
            Finish(game);
            return false;
        }

        DrivePads(frame_);
        session_->Update();
        if (learn_) LearnPending(game);
        pending_.clear();
        if (++frame_ >= frames_) Finish(game);
        return true;
    }

private:
    void DrivePads(int frame)
    {
        for (int p = 0; p < 2; p++) {
            const uint64_t key[3] = {seed_, static_cast<uint64_t>(p), static_cast<uint64_t>(frame / 5)};
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
            SetScriptedPad(p, true, b, 0);
        }
    }

    // Noise is a few isolated words; a real divergence spreads into many. Only
    // frames that differ in a few words are learned, the rest fail the run.
    void LearnPending(SfvGame& game)
    {
        if (pending_.empty()) return;
        std::map<int, int> perFrame;
        for (const auto& d : pending_) perFrame[d.frame]++;
        for (const auto& d : pending_) {
            if (perFrame[d.frame] > kMaxNoiseWordsPerFrame) continue;
            const auto& r = game.Regions()[d.region];
            if (game.AddNoise(r.size, d.offset)) {
                Log("learnnoise: frame %d %s +0x%zx (size %zu)", d.frame, r.name.c_str(), d.offset, r.size);
                learned_++;
            }
        }
        for (const auto& [frame, words] : perFrame) {
            if (words > kMaxNoiseWordsPerFrame && rejected_++ < 8) {
                Log("learnnoise: frame %d differs in %d words: a real divergence, not learned", frame, words);
            }
        }
    }

    void Finish(SfvGame& game)
    {
        SetRoundDirectorPerTick(false, false);
        SetScriptedPad(0, false);
        SetScriptedPad(1, false);
        game.bothPadsLocal = false;
        if (session_) {
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
            int loads = 0;
            for (const auto& e : session_->Recording().events) loads += e.type == ReplayEventType::Rollback;
            const int desyncs = session_->DesyncCount();
            Log("stress: %s, %d frames, distance %d, %d loads, %d regions (%zu bytes), %d desync events, %d differing "
                "words, %.1f ms per frame",
                learn_ ? (rejected_ ? "LEARNED WITH REJECTS" : "LEARNED") : desyncs == 0 ? "PASS" : "FAIL", frame_, distance_, loads, int(game.Regions().size()),
                StateSize(game.Regions()), desyncs, diffWords_, frame_ ? double(ms) / frame_ : 0.0);
            if (learn_) {
                const bool saved = !g_noisePath.empty() && game.SaveNoise(g_noisePath);
                Log("learnnoise: %d new words, %zu in the mask, %s", learned_, game.NoiseWords(),
                    saved ? "saved" : "NOT saved");
            }
            for (const auto& d : session_->Desyncs()) {
                Log("stress:   frame %d checksum %08x, first simulation %08x", d.frame, d.localChecksum, d.remoteChecksum);
            }
        }
        SetFrameDriver(nullptr);
        delete this;
    }

    int frames_, distance_;
    uint64_t seed_;
    bool learn_;
    int frame_ = 0;
    int layout_ = 0;
    int diffWords_ = 0;
    int learned_ = 0;
    int rejected_ = 0;
    struct Diff {
        int frame;
        size_t region, offset;
    };
    std::vector<Diff> pending_;
    static constexpr int kMaxNoiseWordsPerFrame = 16;
    NoTransport transport_;
    std::unique_ptr<NetplaySession> session_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace

void SetNoisePath(const std::wstring& path) { g_noisePath = path; }

void RequestStressTest(int frames, int distance, uint64_t seed, bool learnNoise)
{
    if (CurrentFrameDriver()) {
        Log("stress: another driver is active");
        return;
    }
    distance = distance < 1 ? 1 : distance > 30 ? 30 : distance;
    Log("stress: %d frames, distance %d, seed %llu", frames, distance, static_cast<unsigned long long>(seed));
    SetFrameDriver(new StressDriver(frames, distance, seed, learnNoise));
}

} // namespace sfv::mod
