#pragma once
// In-game determinism test, run from the battle tick hook.
//
//   1. Save the state at a tick boundary.
//   2. Record: drive both virtual pads from a frame-indexed input script for N
//      frames, hashing every region at the start of every tick.
//   3. Restore the saved state, replay the same script, hash again.
//
// If the restored game is a pure function of (snapshot, inputs), both passes
// produce identical hashes. The first frame that differs, the regions that
// differ and their byte ranges go to the log. Polls of the virtual pads per
// tick are counted as well: the test is only meaningful when the game reads
// the pads exactly once per tick.

#include "mod/sfv_game.h"

#include <cstdint>
#include <vector>

namespace sfv::mod {

class SyncTest {
public:
    void Start(int frames, uint64_t seed);
    bool Active() const { return phase_ != Phase::Idle; }
    // At the start of every battle tick, on the game thread, before the tick.
    void OnTick(SfvGame& game);

private:
    enum class Phase { Idle, Record, Replay };

    uint16_t Buttons(int frame, int player, uint8_t* rightTrigger) const;
    void DrivePads(int frame);
    void Finish(SfvGame& game);
    void LogByteDiffs(const std::vector<StateRegion>& regions, const std::vector<uint8_t>& a,
                      const std::vector<uint8_t>& b, int frame) const;

    Phase phase_ = Phase::Idle;
    int frames_ = 0;
    uint64_t seed_ = 0;
    int k_ = 0;
    std::vector<uint8_t> saved_;
    std::vector<uint64_t> recorded_;            // (frames + 1) x regions
    std::vector<std::vector<uint8_t>> states_; // pass 1 states, when small enough
    int firstMismatch_ = -1;
    int mismatchedFrames_ = 0;
    std::vector<uint8_t> mismatchState_;
    uint64_t lastPolls_ = 0;
    int pollHistogram_[4] = {};
};

} // namespace sfv::mod
