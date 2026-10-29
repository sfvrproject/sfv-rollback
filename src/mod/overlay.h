#pragma once
// In-game overlay: Dear ImGui drawn into the game's own swap chain.
//
// IDXGISwapChain::Present and ResizeBuffers are hooked (their addresses come
// from a throwaway device and swap chain made at install). On the game's
// Present the overlay draws into the back buffer, then the frame goes on as
// usual (also through the Steam overlay, which hooks Present too).
//
// Input: window messages reach ImGui through the mod's window procedure
// (control.cpp), queued and fed to ImGui on the render thread (ImGui is not
// thread-safe). While the panel is open the game gets no keyboard or mouse
// messages and sees neutral pads.
//
// Toggle: F1, or Back + Start on a pad. `overlay show|hide|toggle` in the
// command file.
//
// Without the panel, during a session: a status strip, the match HUD (both
// players, ping and jitter, rollbacks, the other's frame rate and link), a
// desync banner, short notices (toasts), and after a match the vote on what
// follows (Rematch, Character select, Leave), which takes the pads. Pads navigate the panel (ImGui gamepad navigation, fed from
// the mod's XInput hook).

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <string>

namespace sfv::mod {

// panelPath: where the panel saves the player's choices (config.h).
bool InstallOverlay(const std::wstring& panelPath);
// After the hooks are disabled and no detour runs (hot reload): releases ImGui
// and its D3D objects.
void ShutdownOverlay();

void SetOverlayVisible(bool visible);
void ToggleOverlay();
// The Debug tab's previews of the match windows (`overlay preview ...`):
// words hud, strip, desync, p1, p2 switch a window on; relay or p2p the link
// they show; vote or select pick
// what P1's and P2's windows show, you1/you2 whose side is this player's,
// toasts adds sample toasts, fade plays the player bars' fade at a match's
// end, off turns every preview off.
void PreviewOverlay(const std::string& words);
// Vsync: off presents every frame at once (sync interval 0) instead of
// waiting for the display's refresh, whatever the game asked for.
void SetVsync(bool on);
bool VsyncOn();
// For the log: the game's own sync interval, the swap chain, and the time
// spent in Present per frame since the last call.
std::string VsyncInfo();
bool OverlayVisible();
// The overlay takes the keyboard, mouse and pads (the game sees neutral pads):
// the panel is open, or the post-match vote waits for this player.
bool OverlayTakesInput();
// The mod's version, shown in the panel's title.
void SetOverlayBuild(const std::string& version);
// Frames the game presented per second, recently.
float RenderFps();

// From the game window's procedure: true when the overlay took the message
// (the game must not see it).
bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
// From the pad poll, with a pad's real state: Back + Start toggles; while the
// panel is open the pads also navigate it (D-pad or left stick, A, B).
void OverlayPad(unsigned user, const XINPUT_GAMEPAD& pad);

} // namespace sfv::mod
