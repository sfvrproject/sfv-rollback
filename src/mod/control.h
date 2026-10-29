#pragma once
// Test automation: virtual XInput pads and a command file.
//
// The mod hooks XInputGetState so up to four virtual pads can be pressed from
// code. Commands are read from sfv_cmd.txt next to the DLL (the file is
// deleted once read), one per line:
//
//   pad <user> <buttons> <holdMs> [gapMs]   press buttons, e.g. "pad 0 a 100"
//                                           buttons: up down left right start
//                                           back lb rb a b x y lt rt, joined by +
//   connect <user> / disconnect <user>      virtual pad presence
//   wait <ms>
//   save / load                             copy-mode savestate (F5 / F9)
//   ringsave / ringload                     game ring savestate (F6 / F10)
//   layout                                  log the rollback layout
//   synctest <frames> [seed]                in-game determinism test (synctest.h)
//   watch <region> <offset> [len]           hardware write watch (watch.h)
//   watchaddr <hexaddr> [len] [rw]          same on any address; rw traps reads too
//   setup <code costume color comment vskill vtrigger> x2 <stage>
//                                           battle setup for the next battle (battle_setup.h)
//   showsetup / clearsetup
//   host [random] [costumes] / join <code> / leave / netstatus      netplay (netplay_driver.h)
//   netsim <latency ms> <jitter ms> <loss %> / mash <seed> / pick ... / stage <n>
//   scan <int> / rescan <int> / scanshow    value scanner (memscan.h)
//   trace <rva>... / tracereport            call tracer (tracer.h)
//   stress <frames> <distance> [seed]       rollback sync test (stress_driver.h)
//   learnnoise <frames> <distance> [seed]   same, learning the noise mask
//   watchreport / unwatch
//
// Pad steps and waits run in order on a timer thread; the other commands are
// queued for the game thread and run at the start of the next battle tick.
// The game ignores pad input while its window is not in the foreground.

#include "core/game.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace sfv::mod {

enum class GameCommand { None, Save, Load, RingSave, RingLoad, Layout, SyncTest, Watch, WatchAddr, WatchReport, Unwatch, Stress, LearnNoise };

struct GameRequest {
    GameCommand command = GameCommand::None;
    int arg0 = 0;
    int arg1 = 0;
    int arg2 = 0;
    uint64_t address = 0;
    bool flag = false;
};

// `virtualPads` pads (from user 0) are connected from the start: the game
// only looks for controllers when it starts or a device arrives. With
// alwaysActive the game believes its window is in front, so it keeps reading
// pads while in the background (two instances on one desktop).
bool InstallControl(const std::wstring& commandFile, int virtualPads, bool alwaysActive);
// The real foreground window, unaffected by alwaysActive.
HWND RealForegroundWindow();
// Puts the game's window procedure back (before unloading).
void RestoreWindowProc();
// While set, Start/Back on every pad and Enter/Escape on the keyboard are
// swallowed: a pause on one side of a netplay match would desync it.
void SetBlockPause(bool block);
// The game's own pad input (UE4, menus and the win screens) sees `buttons`
// (XINPUT_GAMEPAD_*) on the first pad for `ms`, even while a match isolates
// the pads.
void PressUiButtons(uint16_t buttons, int ms);
// Netplay: the game's input manager is cleared after each update (keyboard
// and pads), so local input cannot reach the simulation except through the
// injected inputs.
void SetPadIsolation(bool isolate);
// The last state of pad `user` (real pad plus virtual buttons) as a game input
// value (default button layout), also while isolated.
PackedInput LocalPadInput(int user);
// The local player's input as the game maps it (keyboard and pads, with the
// player's button config: the input manager's pad 0, before netplay clears
// it), with Start; the pad's default layout when the manager is not hooked.
PackedInput MappedLocalInput(int unused);
// Called by the battle tick hook: the next queued game-thread command.
GameRequest TakeGameRequest();

// Scripted pad state, set from the battle tick: while enabled it replaces the
// pad steps for that user.
void SetScriptedPad(int user, bool enabled, uint16_t buttons = 0, uint8_t rightTrigger = 0);
// Number of XInputGetState calls the game has made for `user`.
uint64_t PadPolls(int user);

} // namespace sfv::mod
