#include "mod/config.h"

#include <windows.h>

#include <cwchar>

namespace sfv::mod {

namespace {

std::string Ascii(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(std::size(buf)), path.c_str());
    return buf;
}

int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def)
{
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, path.c_str()));
}

} // namespace

namespace {

// The player's choices in [netplay]: pick (with its variant), stage, input delay,
// own frame rate in the HUD; from the panel also name and spectators. With onlyPresent (the overlay's file) a missing key keeps the value
// read before.
void ReadNetplayChoices(const std::wstring& path, NetplayOptions& n, bool onlyPresent)
{
    const wchar_t* kAbsent = L"\x01";
    auto has = [&](const wchar_t* key) { return !onlyPresent || ReadString(path, L"netplay", key, kAbsent) != kAbsent; };
    if (has(L"input_delay")) n.inputDelay = static_cast<uint8_t>(ReadInt(path, L"netplay", L"input_delay", 2));
    if (has(L"character")) {
        const std::wstring chara = ReadString(path, L"netplay", L"character", L"");
        n.hasPick = !chara.empty();
        if (n.hasPick) {
            const std::string code = Ascii(chara);
            n.pick.character = PackCode(code.c_str());
            n.pick.costume = static_cast<uint16_t>(ReadInt(path, L"netplay", L"costume", 0));
            n.pick.color = static_cast<uint8_t>(ReadInt(path, L"netplay", L"color", 1) - 1);
            n.pick.comment = static_cast<uint8_t>(ReadInt(path, L"netplay", L"comment", 0));
            n.pick.vskill = static_cast<uint8_t>(ReadInt(path, L"netplay", L"vskill", 1) - 1);
            n.pick.vtrigger = static_cast<uint8_t>(ReadInt(path, L"netplay", L"vtrigger", 1) - 1);
            n.pick.variation = static_cast<uint8_t>(ReadInt(path, L"netplay", L"variant", 0));
        }
    }
    // A stage code ("CHN"), "random", or nothing (keep the stage the game has
    // prepared); resolved against the stage table when hosting.
    if (has(L"stage")) n.stageCode = Ascii(ReadString(path, L"netplay", L"stage", L""));
    // Set in the panel (the main file has them in [weyve] and [netplay]).
    if (onlyPresent && has(L"name")) n.name = Ascii(ReadString(path, L"netplay", L"name", L""));
    if (onlyPresent && has(L"allow_spectators")) n.allowSpectators = ReadInt(path, L"netplay", L"allow_spectators", 1) != 0;
    // The Debug tab's server switch, and the servers used before.
    if (onlyPresent && has(L"server")) {
        const std::string server = Ascii(ReadString(path, L"netplay", L"server", L""));
        if (!server.empty()) n.server = server;
    }
    if (onlyPresent && has(L"recent_servers")) {
        n.recentServers.clear();
        const std::string list = Ascii(ReadString(path, L"netplay", L"recent_servers", L""));
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            if (end > at) n.recentServers.push_back(list.substr(at, end - at));
            at = end + 1;
        }
    }
    if (has(L"show_fps")) n.showOwnFps = ReadInt(path, L"netplay", L"show_fps", 0) != 0;
}

void Write(const std::wstring& path, const wchar_t* key, const std::string& value)
{
    const std::wstring wide(value.begin(), value.end());
    WritePrivateProfileStringW(L"netplay", key, wide.c_str(), path.c_str());
}

} // namespace

void SaveNetplayChoices(const std::wstring& path, const NetplayOptions& n, const std::string& stage)
{
    Write(path, L"character", n.hasPick ? UnpackCode(n.pick.character) : "");
    Write(path, L"costume", std::to_string(n.pick.costume));
    Write(path, L"color", std::to_string(n.pick.color + 1));
    Write(path, L"comment", std::to_string(n.pick.comment));
    Write(path, L"vskill", std::to_string(n.pick.vskill + 1));
    Write(path, L"vtrigger", std::to_string(n.pick.vtrigger + 1));
    Write(path, L"variant", std::to_string(n.pick.variation));
    Write(path, L"stage", stage);
    Write(path, L"input_delay", std::to_string(n.inputDelay));
    Write(path, L"name", n.name);
    Write(path, L"allow_spectators", n.allowSpectators ? "1" : "0");
    Write(path, L"show_fps", n.showOwnFps ? "1" : "0");
    Write(path, L"server", n.server);
    std::string recent;
    for (const std::string& s : n.recentServers) recent += (recent.empty() ? "" : ";") + s;
    Write(path, L"recent_servers", recent);
}

Config LoadConfig(const std::wstring& path, const std::wstring& dllDir, const std::wstring& panelPath)
{
    Config c;
    const std::wstring mode = ReadString(path, L"mod", L"mode", L"probe");
    if (mode == L"off") c.mode = Mode::Off;
    else if (mode == L"learn_statemap") c.mode = Mode::LearnStateMap;
    else if (mode == L"netplay") c.mode = Mode::Netplay;
    else if (mode == L"playback") c.mode = Mode::Playback;
    else c.mode = Mode::Probe;

    c.savestateHotkeys = ReadInt(path, L"probe", L"savestate_hotkeys", 1) != 0;
    c.forceOfflineFrames = ReadInt(path, L"probe", L"force_offline_frames", 0) != 0;
    c.forceEnable = ReadInt(path, L"probe", L"force_enable", 0) != 0;
    c.offlineBoot = ReadInt(path, L"boot", L"offline", 1) != 0;
    c.includeSyncCounters = ReadInt(path, L"probe", L"include_sync_counters", 1) != 0;
    c.verbose = ReadInt(path, L"probe", L"verbose", 0) != 0;

    c.localPlayer = ReadInt(path, L"netplay", L"local_player", 0);
    c.inputDelay = ReadInt(path, L"netplay", L"input_delay", 2);
    c.predictionWindow = ReadInt(path, L"netplay", L"prediction_window", 8);

    c.playbackFile = ReadString(path, L"playback", L"file", L"");
    c.stressDistance = ReadInt(path, L"playback", L"stress", 0);

    NetplayOptions& n = c.netplay;
    n.udp = ReadString(path, L"netplay", L"transport", L"weyve") == L"udp";
    n.server = Ascii(ReadString(path, L"weyve", L"server", L""));
    if (n.server.empty()) n.server = kDefaultServer;
    n.name = Ascii(ReadString(path, L"weyve", L"name", L""));
    n.allowSpectators = ReadInt(path, L"netplay", L"allow_spectators", 1) != 0;
    n.udpPort = static_cast<uint16_t>(ReadInt(path, L"netplay", L"udp_port", 47001));
    const std::wstring host = ReadString(path, L"netplay", L"udp_host", L"127.0.0.1");
    n.udpHost = Ascii(host);
    n.lockstepPresentation = ReadInt(path, L"netplay", L"lockstep_presentation", 1) != 0;
    n.lockstepDelay = ReadInt(path, L"netplay", L"lockstep_delay", 1) != 0;
    n.roundEndRollback = ReadInt(path, L"netplay", L"round_end_rollback", 1) != 0;
    n.menuAfterMatch = ReadInt(path, L"netplay", L"menu_after_match", 1) != 0;
    n.reconcileStart = ReadInt(path, L"netplay", L"reconcile_start", 1) != 0;
    n.fullRecordings = ReadInt(path, L"netplay", L"full_recordings", 0) != 0;
    n.skipSaves = ReadInt(path, L"netplay", L"skip_saves", 1) != 0;
    n.verifyLoads = ReadInt(path, L"netplay", L"verify_loads", 1) != 0;
    ReadNetplayChoices(path, n, false);
    if (!panelPath.empty()) ReadNetplayChoices(panelPath, n, true);
    n.randomSetup = ReadInt(path, L"netplay", L"random", 0) != 0;
    n.conditions.latencyMs = ReadInt(path, L"netsim", L"latency_ms", 0);
    n.conditions.jitterMs = ReadInt(path, L"netsim", L"jitter_ms", 0);
    n.conditions.loss = ReadInt(path, L"netsim", L"loss_pct", 0) / 100.0;

    c.virtualPads = ReadInt(path, L"control", L"virtual_pads", 0);
    c.alwaysActive = ReadInt(path, L"control", L"always_active", 0) != 0;

    c.noiseFile = ReadString(path, L"mod", L"noise_file", L"");
    c.reportDir = ReadString(path, L"mod", L"report_dir", L"");
    if (c.reportDir.empty()) c.reportDir = dllDir + L"\\SFVR\\matches";
    c.updateUrl = ReadString(path, L"update", L"url", L"");
    c.updateCheck = ReadInt(path, L"update", L"check", 1) != 0;
    return c;
}

const char* ModeName(Mode mode)
{
    switch (mode) {
    case Mode::Off: return "off";
    case Mode::Probe: return "probe";
    case Mode::LearnStateMap: return "learn_statemap";
    case Mode::Netplay: return "netplay";
    case Mode::Playback: return "playback";
    }
    return "?";
}

} // namespace sfv::mod
