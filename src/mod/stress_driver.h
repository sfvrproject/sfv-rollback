#pragma once
// In-game rollback sync test: the "stress <frames> <distance> [seed]" command.
//
// Runs a GekkoNet stress session (core NetplaySession, stress mode) against
// the live battle: every frame it loads the state from <distance> frames back
// and resimulates to the present with the same inputs, and compares the
// checksums of resimulated frames with the first simulation. Both virtual pads
// mash pseudo-random inputs meanwhile. This is the same save/load/advance
// path netplay uses, at the worst case of a rollback on every frame.

#include "mod/battle_driver.h"

#include <cstdint>

namespace sfv::mod {

// Starts a test on the next battle frame. Called from any thread. With
// learnNoise, words that resimulate differently are added to the game's noise
// mask (and saved to noisePath at the end) instead of failing the test.
void RequestStressTest(int frames, int distance, uint64_t seed, bool learnNoise = false);
void SetNoisePath(const std::wstring& path);

} // namespace sfv::mod
