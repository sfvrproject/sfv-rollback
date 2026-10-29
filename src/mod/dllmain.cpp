#include "mod/battle_driver.h"
#include "mod/control.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/overlay.h"
#include "mod/watch.h"

#include <MinHook.h>
#include <windows.h>

namespace sfv::mod {
DWORD WINAPI ModMain(LPVOID self);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Hooks and file I/O happen off the loader lock.
        if (HANDLE t = CreateThread(nullptr, 0, sfv::mod::ModMain, module, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}

// Asked by the loader proxy before a hot reload: not while a netplay session
// is on (the reload would end it for both players), and not while a frame
// driver still runs the battle's frames (after a leave, until the game is
// back at the menus): the game thread can be inside the old driver's frame
// longer than the shutdown waits, and went on in the freed driver after the
// reload (crash, 2026-10-28).
extern "C" __declspec(dllexport) bool SfvCanUnload()
{
    using namespace sfv::mod;
    const NetView::Phase phase = NetViewNow().phase;
    if (phase != NetView::Phase::Idle && phase != NetView::Phase::Ended) {
        Log("hot reload refused: a netplay session is on (leave it first)");
        return false;
    }
    if (CurrentFrameDriver()) {
        Log("hot reload refused: the battle's frames are still driven (wait for the menus)");
        return false;
    }
    return true;
}

// Called by the loader proxy before it unloads this DLL for a hot reload.
extern "C" __declspec(dllexport) void SfvShutdown()
{
    using namespace sfv::mod;
    g_stopping = true;
    // Its threads and frame driver go first, while the hooks still run.
    NetLeave();
    RemoveWatchHandler();
    RestoreWindowProc();
    MH_DisableHook(MH_ALL_HOOKS);
    // Threads may be in the middle of a walk over every UE4 object (about a
    // second each).
    for (int i = 0; i < 500 && (g_inDetour > 0 || g_threads > 0); i++) Sleep(10);
    // A detour may have returned from its guard but not yet from the function.
    Sleep(50);
    // No Present runs our code any more: ImGui and its D3D objects can go.
    ShutdownOverlay();
    MH_Uninitialize();
    Log("shut down for reload (%d detours, %d threads still active)", g_inDetour.load(), g_threads.load());
    LogClose();
}
