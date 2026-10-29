#pragma once
// Finds state that the battle ticks change but its snapshot does not restore:
// saves the snapshot and a copy of every UObject, runs ticks, loads the
// snapshot, and copies the objects again. What still differs (minus what
// changes without any ticks: render and audio threads) is outside the
// snapshot. Logs the objects, worst first.

namespace sfv::mod {

class SfvGame;

// Runs once at the next battle frame (no netplay session may be running).
// allMemory: every writable page instead of the UObjects (slower; also finds
// heap arrays and plain C++ objects).
void RequestOutsideProbe(SfvGame* game, int ticks, bool allMemory = false);

} // namespace sfv::mod
