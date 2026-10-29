#include "mod/synctest.h"

#include "core/replay_diff.h"
#include "core/state.h"
#include "mod/control.h"
#include "mod/log.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstring>

namespace sfv::mod {

namespace {

// Neutral frames at both ends so the pads hold the same state when each pass
// starts: edge detection outside the snapshot then sees the same history.
constexpr int kNeutralFrames = 12;
// Keep every pass-1 state (for byte diffs) only for short tests.
constexpr int kMaxKeptStates = 240;

} // namespace

void SyncTest::Start(int frames, uint64_t seed)
{
    if (Active()) {
        Log("synctest: already running");
        return;
    }
    frames_ = std::clamp(frames, 2 * kNeutralFrames + 1, 36000);
    seed_ = seed;
    k_ = 0;
    firstMismatch_ = -1;
    mismatchedFrames_ = 0;
    mismatchState_.clear();
    std::fill(std::begin(pollHistogram_), std::end(pollHistogram_), 0);
    phase_ = Phase::Record;
    Log("synctest: %d frames, seed %llu", frames_, static_cast<unsigned long long>(seed));
}

uint16_t SyncTest::Buttons(int frame, int player, uint8_t* rightTrigger) const
{
    *rightTrigger = 0;
    // Seed 0 keeps both pads neutral: separates input-driven state from the rest.
    if (seed_ == 0 || frame < kNeutralFrames || frame >= frames_ - kNeutralFrames) return 0;
    const uint64_t key[3] = {seed_, static_cast<uint64_t>(player), static_cast<uint64_t>(frame / 5)};
    const uint64_t h = Hash64(key, sizeof(key));
    uint16_t b = 0;
    switch (h % 8) {
    case 0: case 1: b |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 2: case 3: b |= XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 4: b |= XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 5: b |= XINPUT_GAMEPAD_DPAD_UP; break;
    default: break;
    }
    // LP MP HP LK MK HK on the default pad layout.
    static const uint16_t kAttack[] = {XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                       XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, 0};
    if ((h >> 8) % 3 == 0) {
        const int attack = static_cast<int>((h >> 16) % 6);
        if (attack == 5) *rightTrigger = 255;
        else b |= kAttack[attack];
    }
    return b;
}

void SyncTest::DrivePads(int frame)
{
    for (int p = 0; p < 2; p++) {
        uint8_t rt;
        const uint16_t b = Buttons(frame, p, &rt);
        SetScriptedPad(p, true, b, rt);
    }
}

void SyncTest::OnTick(SfvGame& game)
{
    if (phase_ == Phase::Idle) return;
    const auto& regions = game.Regions();
    const size_t count = regions.size();
    const size_t size = StateSize(regions);

    // Pad reads since the previous tick; each tick should see exactly one.
    const uint64_t polls = PadPolls(0);
    if (k_ > 0) pollHistogram_[std::min<uint64_t>(polls - lastPolls_, 3)]++;
    lastPolls_ = polls;

    if (phase_ == Phase::Record && k_ == 0) {
        saved_.resize(size);
        SaveRegions(regions, saved_.data());
        recorded_.assign((size_t(frames_) + 1) * count, 0);
        states_.clear();
        if (frames_ <= kMaxKeptStates) states_.resize(size_t(frames_) + 1);
    }

    std::vector<uint64_t> row(count);
    HashRegions(regions, row.data());

    if (phase_ == Phase::Record) {
        std::memcpy(&recorded_[size_t(k_) * count], row.data(), count * sizeof(uint64_t));
        if (!states_.empty()) {
            states_[k_].resize(size);
            SaveRegions(regions, states_[k_].data());
        }
    }
    else if (std::memcmp(&recorded_[size_t(k_) * count], row.data(), count * sizeof(uint64_t)) != 0) {
        mismatchedFrames_++;
        if (firstMismatch_ < 0) {
            firstMismatch_ = k_;
            mismatchState_.resize(size);
            SaveRegions(regions, mismatchState_.data());
        }
        // How the difference evolves: a few sample frames, with context.
        const int since = k_ - firstMismatch_;
        if (size_t(k_) < states_.size() && (since <= 2 || k_ == 30 || k_ == 100 || k_ == frames_ - 1)) {
            std::vector<uint8_t> now(size);
            SaveRegions(regions, now.data());
            LogByteDiffs(regions, states_[k_], now, k_);
        }
    }

    if (k_ < frames_) {
        DrivePads(k_);
        k_++;
        return;
    }

    if (phase_ == Phase::Record) {
        LoadRegions(regions, saved_.data());
        game.OnStateLoaded(0);
        phase_ = Phase::Replay;
        k_ = 0;
        Log("synctest: recorded %d frames, restored, replaying", frames_);
        // The tick now runs from the restored state: treat it as frame 0.
        OnTick(game);
        return;
    }
    Finish(game);
}

void SyncTest::LogByteDiffs(const std::vector<StateRegion>& regions, const std::vector<uint8_t>& a,
                            const std::vector<uint8_t>& b, int frame) const
{
    std::vector<RegionInfo> infos;
    std::vector<size_t> offsets;
    size_t base = 0;
    for (const auto& r : regions) {
        infos.push_back({r.name, r.size, {}});
        offsets.push_back(base);
        base += r.size;
    }
    for (const auto& br : DiffStates(infos, a, b, 16)) {
        // 16 bytes of context starting 8 before the difference, both passes.
        const size_t regionStart = offsets[br.region];
        const size_t from = regionStart + (br.offset >= 8 ? br.offset - 8 : 0);
        const size_t to = std::min(regionStart + regions[br.region].size, from + 24);
        char ha[80] = {}, hb[80] = {};
        int na = 0, nb = 0;
        for (size_t i = from; i < to; i++) {
            na += std::snprintf(ha + na, sizeof(ha) - na, "%02x", a[i]);
            nb += std::snprintf(hb + nb, sizeof(hb) - nb, "%02x", b[i]);
        }
        Log("  frame %d %s +0x%zx (%zu bytes) context from +0x%zx:", frame, regions[br.region].name.c_str(), br.offset,
            br.length, from - regionStart);
        Log("    pass1 %s", ha);
        Log("    pass2 %s", hb);
    }
}

void SyncTest::Finish(SfvGame& game)
{
    phase_ = Phase::Idle;
    SetScriptedPad(0, false);
    SetScriptedPad(1, false);

    Log("synctest: pad polls per tick: 0:%d 1:%d 2:%d 3+:%d", pollHistogram_[0], pollHistogram_[1], pollHistogram_[2],
        pollHistogram_[3]);
    if (firstMismatch_ < 0) {
        Log("synctest: PASS, %d frames reproduced bit for bit after restore", frames_);
        return;
    }
    Log("synctest: FAIL, first mismatch at frame %d, %d of %d frames differ", firstMismatch_, mismatchedFrames_,
        frames_);

    const auto& regions = game.Regions();
    const size_t count = regions.size();
    // Recompute which regions differ at the first mismatching frame.
    std::vector<uint64_t> row(count);
    HashSavedState(regions, mismatchState_.data(), row.data());
    const uint64_t* expected = &recorded_[size_t(firstMismatch_) * count];
    int listed = 0;
    for (size_t i = 0; i < count && listed < 24; i++) {
        if (row[i] != expected[i]) {
            Log("  region %s (%zu bytes) differs", regions[i].name.c_str(), regions[i].size);
            listed++;
        }
    }
    if (size_t(firstMismatch_) < states_.size()) {
        std::vector<RegionInfo> infos;
        for (const auto& r : regions) infos.push_back({r.name, r.size, {}});
        size_t base = 0;
        std::vector<size_t> offsets;
        for (const auto& r : regions) {
            offsets.push_back(base);
            base += r.size;
        }
        const auto& a = states_[firstMismatch_];
        for (const auto& br : DiffStates(infos, a, mismatchState_, 48)) {
            const size_t at = offsets[br.region] + br.offset;
            uint64_t va = 0, vb = 0;
            std::memcpy(&va, &a[at], std::min<size_t>(8, a.size() - at));
            std::memcpy(&vb, &mismatchState_[at], std::min<size_t>(8, mismatchState_.size() - at));
            Log("  %s +0x%zx %zu byte(s): %016llx -> %016llx", regions[br.region].name.c_str(), br.offset, br.length,
                static_cast<unsigned long long>(va), static_cast<unsigned long long>(vb));
        }
    }
}

} // namespace sfv::mod
