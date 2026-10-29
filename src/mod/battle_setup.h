#pragma once
// The game's battle settings: who plays what, in which costume and colour,
// with which V-Skill, V-Trigger and V-Comment, on which stage.
//
// Path: engine global (rva 0x3D878C8) -> +0x1E0 game instance ->
// vtable[0x238/8]() = battle-setting object; its +0x30 points at the battle
// data block, which starts with match fields and holds four player records of
// 0x590 bytes (record i at block + i*0x590, fields below relative to that).
// Local Versus fills the block from the menus; a match setup written into it
// when character select ends (BattleSetting::CheckPlayers) and again when the
// battle actor reads it (before assets load) replaces the menu choices.

#include "core/game.h"
#include "mod/image.h"

#include <cstdint>
#include <string>

namespace sfv::mod {

namespace battledata {
constexpr uint32_t kMode = 0x00;    // int, 1 = local Versus (3 when a jump builds the data from defaults)
constexpr uint32_t kRounds = 0x04;  // int
constexpr uint32_t kTimer = 0x0C;   // int, frames (99 s = 5940)
constexpr uint32_t kVersusFlag = 0x40; // int, 1 in local Versus, 0 in the defaults
constexpr uint32_t kStage = 0x60;   // int, stage and setting id (0 = The Grid Alternative); read only, see ApplyMatchSetup
constexpr uint32_t kRecordStride = 0x590;
constexpr uint32_t kCharaCode = 0x90; // char[4] + wchar_t[4], e.g. "KEN"
constexpr uint32_t kCharaVersion = 0x9C;
constexpr uint32_t kCostume = 0xB0;
constexpr uint32_t kColor = 0xB4; // 0-based
constexpr uint32_t kComment = 0xD4; // -1 = Auto
constexpr uint32_t kVariation = 0xD8;
constexpr uint32_t kVTrigger = 0xDC;
constexpr uint32_t kCriticalArt = 0xE0;
constexpr uint32_t kVSkill = 0xE4;
// From KWBattlePlayerSetting's setters (rva 0x7C8880 and on): the wins and
// the win streak the battle starts the player with (the battle's "Wins"
// counter and the results screen's "Wins" / "Win Streak" count on from
// them), who controls the player (EKWCtrlType; 0 a pad, 3 the CPU: the
// defaults a jump builds make P2 a CPU) and with which pad (the input
// manager's pad 0 or 1, rva 0x443D20).
constexpr uint32_t kWins = 0x210;
constexpr uint32_t kWinStreak = 0x214;
constexpr uint32_t kCtrlType = 0x21C;
constexpr uint32_t kPadId = 0x220;
// The costume's dress state per round (8 ints; the menus' held-button
// variant), and whether the battle uses them (rva 0x7C71C0, 0x7CB080).
constexpr uint32_t kDressStates = 0x1A8;
constexpr uint32_t kUseDressState = 0x1DC;
// Per-character unique params (8 ints; SetUniqueParam, rva 0x7CB050): index 2
// = 2 is Urien's thong (the game sets it so for URN, rva 0x5C22F7), index 0
// = 1 Akira's helmet off on her Story costume.
constexpr uint32_t kUniqueParams = 0x128;
} // namespace battledata

bool InstallBattleSetup(const GameImage& image);

// The battle data block, or nullptr outside the menus/battle that use it.
uint8_t* BattleData();
bool ReadMatchSetup(MatchSetup* out);
bool ApplyMatchSetup(const MatchSetup& setup);

// Applied by the hooks the next time a battle is set up, until cleared.
void SetPendingSetup(const MatchSetup& setup);
void ClearPendingSetup();
bool HasPendingSetup();
// Research: a SetUniqueParam value (player 0/1, index 0-7; -1 clears) for
// the next battles.
void SetUniqueParamOverride(int player, int index, int value);
// Counts battle setups seen (CheckPlayers calls) and battles created
// (ReadSettings calls), to notice a jump into battle happened.
int BattleSetupCount();
int BattleCreationCount();

std::string DescribeSetup(const MatchSetup& setup);
void LogBattleSetup(const char* when);

} // namespace sfv::mod
