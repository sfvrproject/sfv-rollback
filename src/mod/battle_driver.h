#pragma once
// Takes over the battle's frame loop.
//
// BattleFrame::Update runs once per rendered frame on the game thread and
// performs that frame's battle ticks. With a frame driver installed, the hook
// calls the driver instead; the driver runs ticks itself through
// SfvGame::Advance (one tick with given inputs each), loads and saves state
// in between, or runs none to stall. InputUnit::RecordFrame is hooked so each
// tick records exactly the inputs the driver chose.

#include "mod/sfv_game.h"

namespace sfv::mod {

class IFrameDriver {
public:
    virtual ~IFrameDriver() = default;
    // Once per rendered battle frame, on the game thread, between ticks.
    // Return false to let the game run the frame normally.
    virtual bool OnFrame(SfvGame& game) = 0;
};

bool InstallBattleDriver(const GameImage& image, SfvGame* game);
// The driver used from the next frame on; nullptr gives the frame loop back
// to the game. The caller keeps ownership.
void SetFrameDriver(IFrameDriver* driver);
IFrameDriver* CurrentFrameDriver();
// Research (`regions <size>`): logs the snapshot regions of that size with their addresses.
void LogRegionsOfSize(size_t size);
// The match setup's seed, which the fighters' random draws in driven ticks
// start from (the same on every machine of a match).
void SetMatchDrawSeed(uint64_t seed);
// Research: logs the next `count` random stream draws made in driven ticks
// (tick, stream, caller).
void LogRandomDraws(int count);
// Research: logs the game's own per-pad input values for the next `ticks`
// ticks, when they change.
void LogGameInputs(int ticks);

} // namespace sfv::mod
