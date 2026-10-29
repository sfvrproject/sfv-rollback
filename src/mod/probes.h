#pragma once
// Phase 1-2 instrumentation of the game's rollback machinery.

#include "mod/config.h"
#include "mod/sfv_game.h"

namespace sfv::mod {

// Installs the hooks. `game` and `stateMap` must outlive the process.
bool InstallProbes(const GameImage& image, const Config& config, SfvGame* game, StateMap* stateMap,
                   const std::wstring& stateMapPath);

} // namespace sfv::mod
