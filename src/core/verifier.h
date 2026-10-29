#pragma once
// Deterministic playback of a recorded match.
//
// The verifier starts the recorded match setup, feeds the recorded inputs frame
// by frame with no network involved, and compares the per-region hashes after
// every frame with the ones the player's machine recorded. Possible outcomes:
//
//   Match          the simulation reproduces the recording bit for bit
//   SetupMismatch  the state before frame 0 already differs: the match setup
//                  (characters, stage, options, seed) did not reach the same
//                  starting state, so the bug is in setup sync, not simulation
//   Diverged       frame N differs although the inputs are the recorded ones:
//                  the recording machine's simulation was not deterministic
//                  (or it ran a different build); the region list says where
//   StressMismatch with rollback stress on, resimulating frames after a load
//                  gave a different result than simulating them once: state
//                  that lives outside the snapshot, or code that behaves
//                  differently while rolling back
//
// Rollback stress replays the real inputs of a reported match instead of the
// random inputs of a sync test, so it reproduces the situation the player hit.

#include "core/game.h"
#include "core/replay_file.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sfv {

struct VerifyOptions {
    // Every `stressEvery` frames, load the state from `stressDistance` frames
    // earlier and resimulate up to the current frame. 0 disables.
    int stressDistance = 0;
    int stressEvery = 1;
    // Stop after this frame (inclusive); -1 plays the whole recording.
    int lastFrame = -1;
    // Only trust frames the recording marks as confirmed.
    bool confirmedOnly = true;
    // Called for every simulated frame, e.g. to drive a progress display.
    std::function<void(int frame)> onFrame;
};

struct RegionMismatch {
    size_t region = 0;
    uint64_t expected = 0;
    uint64_t actual = 0;
};

struct VerifyResult {
    enum class Status { Match, SetupMismatch, Diverged, StressMismatch, Unsupported };

    Status status = Status::Match;
    int frame = -1;          // first bad frame (-1 = before frame 0)
    int framesChecked = 0;
    std::vector<RegionMismatch> regions;
    std::string message;
    // The state the verifier produced at the bad frame, and for a stress
    // mismatch the state it produced on the straight run, for byte diffs.
    std::vector<uint8_t> actualState;
    std::vector<uint8_t> referenceState;
};

const char* ToString(VerifyResult::Status status);

VerifyResult VerifyReplay(IGame& game, const ReplayFile& replay, const VerifyOptions& options = {});

} // namespace sfv
