#include "builtin_noise.h"
#include "core/replay_file.h"
#include "core/state.h"
#include "mod/battle_driver.h"
#include "mod/battle_setup.h"
#include "mod/boot.h"
#include "mod/config.h"
#include "mod/control.h"
#include "mod/flow.h"
#include "mod/overlay.h"
#include "mod/image.h"
#include "mod/netplay_driver.h"
#include "mod/lifecycle.h"
#include "mod/files.h"
#include "mod/log.h"
#include "mod/probes.h"
#include "mod/sfv_game.h"
#include "mod/updater.h"
#include "mod/status.h"
#include "mod/stress_driver.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace sfv::mod {

namespace {

constexpr const char* kModVersion = "0.1.1";

std::wstring DirectoryOf(HMODULE module)
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::string Narrow(const std::wstring& w)
{
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty()) s.pop_back();
    return s;
}

} // namespace

DWORD WINAPI ModMain(LPVOID self)
{
    ThreadGuard running;
    const std::wstring dir = DirectoryOf(static_cast<HMODULE>(self));
    // Several game instances can share the folder; the loader numbers them.
    // Instance 0 uses the plain file names, instance N appends ".N".
    wchar_t instanceText[8] = L"0";
    GetEnvironmentVariableW(L"SFV_INSTANCE", instanceText, 8);
    const int instance = _wtoi(instanceText);
    const std::wstring suffix = instance == 0 ? L"" : L"." + std::to_wstring(instance);
    // The event log in the SFVR folder players share (files.h); the last
    // run's moves aside first.
    SetDataDir(dir + L"\\SFVR");
    EnsureDirectory(LogsDir());
    EnsureDirectory(MatchesDir());
    RotateLog(LogsDir(), L"sfvr" + suffix);
    LogOpen(LogsDir() + L"\\sfvr" + suffix + L".log");
    std::wstring iniPath = dir + L"\\sfv_rollback" + suffix + L".ini";
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) iniPath = dir + L"\\sfv_rollback.ini";
    const std::wstring panelPath = dir + L"\\sfv_rollback.panel" + suffix + L".ini";
    const Config config = LoadConfig(iniPath, dir, panelPath);
    Log("SFVR %s, instance %d, mode %s", kModVersion, instance, ModeName(config.mode));
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModMain), &module);
        Log("module at %p (for scripts/symbolize.py)", static_cast<void*>(module));
    }
    if (config.mode == Mode::Off) return 0;

    // We are loaded during the exe's import resolution, before the SteamStub
    // entry point has run. steam_api64.dll is delay-loaded by the real game
    // code, so its arrival means the stub is done and the game is running.
    for (int i = 0; i < 1200 && !GetModuleHandleW(L"steam_api64.dll"); i++) Sleep(100);
    if (!GetModuleHandleW(L"steam_api64.dll")) {
        Log("steam_api64.dll never loaded; not hooking");
        return 0;
    }

    static GameImage image;
    std::string why;
    if (!OpenGameImage(&image, &why)) {
        Log("not hooking: %s", why.c_str());
        return 0;
    }
    Log("game image at %p verified", reinterpret_cast<void*>(image.base));
    SetStatusInstance(instance);

    static StateMap stateMap;
    const std::wstring stateMapPath = dir + L"\\sfv_statemap.bin";
    const bool haveMap = config.mode != Mode::LearnStateMap && stateMap.Load(stateMapPath);
    Log("state map: %s", haveMap ? "loaded" : config.mode == Mode::LearnStateMap ? "learning" : "none (hashes include pointers)");

    static SfvGame game(image);
    // No AddressTest for the checksums: whether a value is an address in use
    // is a property of one process, so a value equal on both machines could be
    // masked on one side only. Pointer fields are masked by the shared noise
    // file instead (sfvr peers --pointers), identically on both machines.
    if (haveMap) game.SetStateMap(&stateMap);
    // The noise mask is built in, so every copy of a build masks the same
    // words; [mod] noise_file swaps in a file (for learning new words: stress
    // and learnnoise write to it).
    const std::wstring noisePath = dir + L"\\" + (config.noiseFile.empty() ? L"sfv_noise.txt" : config.noiseFile);
    SetNoisePath(noisePath);
    if (!config.noiseFile.empty() && game.LoadNoise(noisePath)) {
        Log("noise mask: %s (%zu words; not the built-in one: peers need the same file)", Narrow(config.noiseFile).c_str(),
            game.NoiseWords());
    }
    else {
        if (!config.noiseFile.empty()) Log("noise mask: cannot read %s; the built-in one instead", Narrow(config.noiseFile).c_str());
        game.LoadNoiseText(reinterpret_cast<const char*>(kBuiltinNoise), sizeof(kBuiltinNoise));
        Log("noise mask: built in (%zu words)", game.NoiseWords());
    }

    if (!InstallProbes(image, config, &game, &stateMap, stateMapPath)) return 0;
    InstallControl(dir + L"\\sfv_cmd" + suffix + L".txt", config.virtualPads, config.alwaysActive);
    InstallOverlay(panelPath);
    SetOverlayBuild(std::string("SFVR ") + kModVersion);
    InstallUpdater(dir, kModVersion, Narrow(config.updateUrl), config.updateCheck);
    InstallBattleSetup(image);
    InstallBootProbes(image, config.verbose);
    InstallFreePlayGuard(image);
    InstallFlowProbes(image, config.offlineBoot);
    WatchLoginStatus();
    InstallBattleDriver(image, &game);
    InstallNetplay(&game, config.reportDir, kModVersion, config.netplay);

    switch (config.mode) {
    case Mode::Netplay:
    case Mode::Playback: {
        if (config.mode == Mode::Playback) {
            ReplayFile rf;
            std::string error;
            if (LoadReplay(Narrow(config.playbackFile), &rf, &error)) {
                Log("playback: %s, %d frames, build %s (this build %s)", Narrow(config.playbackFile).c_str(),
                    rf.FrameCount(), rf.gameBuild.c_str(), game.BuildId().c_str());
            }
            else {
                Log("playback: %s", error.c_str());
            }
        }
        // NetplaySession and VerifyReplay drive the game through
        // SfvGame::BeginMatch/ReadLocalInput/Advance, which need the phase 3
        // input and tick hooks. Until those exist the mod stays in probe mode.
        Log("%s needs the phase 3 input and tick hooks; running probes only", ModeName(config.mode));
        break;
    }
    default:
        break;
    }
    return 0;
}

} // namespace sfv::mod
