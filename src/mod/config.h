#pragma once
// sfv_rollback.ini, next to the DLL. See sfv_rollback.ini.example.

#include "mod/netplay_driver.h"

#include <cstdint>
#include <string>

namespace sfv::mod {

enum class Mode {
    Off,           // load, verify the exe, do nothing
    Probe,         // phase 1-2: log the rollback machinery, savestate hotkeys
    LearnStateMap, // record which state words hold pointers, write sfv_statemap.bin
    Netplay,       // phase 6: needs the input and tick hooks (not found yet)
    Playback,      // verify an .sfvr in local Versus: needs the same hooks
};

struct Config {
    Mode mode = Mode::Probe;

    // probe
    bool savestateHotkeys = true;
    bool forceOfflineFrames = false; // open snapshot frames offline (see probes.cpp)
    bool forceEnable = false;        // set RollbackUnit::enabled offline
    bool includeSyncCounters = true; // snapshot the battle sync frame counters too
    bool verbose = false;            // region layouts, stacks and tick summaries

    // netplay
    int localPlayer = 0;
    int inputDelay = 2;
    int predictionWindow = 8;

    // playback
    std::wstring playbackFile;
    int stressDistance = 0;

    std::wstring reportDir; // match recordings and logs; default <dll dir>\SFVR\matches (files.h)

    // [update] (updater.h): the site's update info, and whether to check at start
    std::wstring updateUrl;
    bool updateCheck = true;

    NetplayOptions netplay; // [netplay] and [netsim]

    int virtualPads = 0; // XInput pads driven by sfv_cmd.txt, connected at startup
    bool alwaysActive = false; // keep reading pads while the window is in the background
    bool offlineBoot = true;   // skip the server login: from the title straight to the main menu (flow.h)
};

// panelPath: the overlay's saved choices (sfv_rollback.panel[.N].ini); its
// [netplay] keys override the main file's.
Config LoadConfig(const std::wstring& path, const std::wstring& dllDir, const std::wstring& panelPath = L"");
// Writes the player's choices (pick, stage, delay, prediction window, name,
// spectators allowed, own frame rate shown) to the overlay's file. `stage`: a code, "random" or "" (as prepared).
void SaveNetplayChoices(const std::wstring& path, const NetplayOptions& n, const std::string& stage);
const char* ModeName(Mode mode);

} // namespace sfv::mod
