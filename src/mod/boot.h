#pragma once
// Booting without the game's servers.
//
// The title's InitialState logs in to Capcom's servers (about 35 s when they
// cannot be reached), then StreetNameRegisterState shows the login result
// dialogs, which wait for their timeouts before taking input; picking
// "offline" in the second one leads to the main menu.
//
// Offline boot (flow.h, [boot] offline=1): InitialState runs its local
// preparation steps, then where it would start the login it takes the game's
// own "continue offline" step to the main menu instead (about 25 s from
// launch, no server requests, no dialogs).
//
// Diagnostics: the online identity's login status (EKWLoginStatus: 0 not
// logged in, 1 using the local profile, 2 logged in), and with logNetwork the
// network calls (Winsock, WinINet, WinHTTP) with how long they took.

#include "mod/image.h"

#include <string>

namespace sfv::mod {

bool InstallBootProbes(const GameImage& image, bool logNetwork);
// While the mod is loaded every character stays playable: the battles' free
// play check (sfv_offsets.h kInFreePlayPeriod) always finds the period
// running. Offline the game has no entitlements, and from 2026-10-29 its free
// play data had run out, so every battle the mod started (Ryu against Ken
// too) ended with "This character's free play period has ended".
bool InstallFreePlayGuard(const GameImage& image);
// Research (`freeplay on|off|fail`): off, the game's own answer stands; fail
// answers "ended" (the dialog's trigger, for testing).
void SetFreePlayGuard(const std::string& mode);
// Offline boot: the title's InitialState has just started. On the next frame,
// takes the game's own "play offline" route out of it (rva 0x5E4ED0: online
// mode off, then the main menu) before the login gets going, and turns off the
// online subsystem's login (as picking "offline" in the login dialog does).
void GoOfflineFromTitle();
// Online identity login status (see above); 0 also without an online
// subsystem. Game thread.
int LoginStatus();
// Logs the login status and every change of it (game thread, until unloaded).
void WatchLoginStatus();

} // namespace sfv::mod
