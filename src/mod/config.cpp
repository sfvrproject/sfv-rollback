#include "mod/config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

std::string Ascii(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

// A value as UTF-8, read from the file itself: Windows' ini functions read a
// file without a BOM in the system's code page, which mangles UTF-8 (what
// Notepad saves). A UTF-16 file (BOM) works too. "" when missing; `found`
// says whether the key is there.
std::string ReadUtf8(const std::wstring& path, const std::string& section, const std::string& key, bool* found = nullptr)
{
    if (found) *found = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring w(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string u(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
        bytes = u;
    }
    else if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        bytes.erase(0, 3);
    }
    auto trim = [](std::string s) {
        const size_t a = s.find_first_not_of(" \t\r");
        const size_t b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    auto same = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; };
    bool inSection = false;
    for (size_t at = 0; at < bytes.size();) {
        size_t end = bytes.find('\n', at);
        if (end == std::string::npos) end = bytes.size();
        const std::string line = trim(bytes.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            inSection = close != std::string::npos && same(trim(line.substr(1, close - 1)), section);
            continue;
        }
        const size_t eq = line.find('=');
        if (!inSection || eq == std::string::npos || !same(trim(line.substr(0, eq)), key)) continue;
        if (found) *found = true;
        return trim(line.substr(eq + 1));
    }
    return "";
}

std::string FromHex(const std::string& hex)
{
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
    return out;
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
    // The panel keeps a name outside ASCII as hex (name_utf8): Windows' ini
    // functions write a file without a BOM in the system's code page.
    if (onlyPresent) {
        bool found = false;
        const std::string hex = ReadUtf8(path, "netplay", "name_utf8", &found);
        if (found && !hex.empty()) n.name = FromHex(hex);
        else if (has(L"name")) n.name = ReadUtf8(path, "netplay", "name");
    }
    if (onlyPresent && has(L"allow_spectators")) n.allowSpectators = ReadInt(path, L"netplay", L"allow_spectators", 1) != 0;
    // The Debug tab's server switch, and the servers used before.
    if (onlyPresent && has(L"server")) {
        const std::string server = Ascii(ReadString(path, L"netplay", L"server", L""));
        if (!server.empty()) n.server = n.savedServer = server;
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
    // The character's loadout, for when it is picked again (ReadLoadouts):
    // costume, color, comment, V-Skill, V-Trigger, variant as stored above.
    if (n.hasPick) {
        const std::string code = UnpackCode(n.pick.character);
        const std::string loadout = std::to_string(n.pick.costume) + " " + std::to_string(n.pick.color + 1) + " " +
                                    std::to_string(n.pick.comment) + " " + std::to_string(n.pick.vskill + 1) + " " +
                                    std::to_string(n.pick.vtrigger + 1) + " " + std::to_string(n.pick.variation);
        WritePrivateProfileStringW(L"loadouts", std::wstring(code.begin(), code.end()).c_str(),
                                   std::wstring(loadout.begin(), loadout.end()).c_str(), path.c_str());
    }
    Write(path, L"stage", stage);
    Write(path, L"input_delay", std::to_string(n.inputDelay));
    // ASCII as it is; anything else as hex in name_utf8 (see ReadNetplayChoices).
    const bool ascii = std::all_of(n.name.begin(), n.name.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
    std::string hex;
    for (unsigned char ch : n.name) {
        char two[3];
        std::snprintf(two, sizeof(two), "%02x", ch);
        hex += two;
    }
    Write(path, L"name", ascii ? n.name : "");
    Write(path, L"name_utf8", ascii ? "" : hex);
    Write(path, L"allow_spectators", n.allowSpectators ? "1" : "0");
    Write(path, L"show_fps", n.showOwnFps ? "1" : "0");
    Write(path, L"server", n.savedServer);
    std::string recent;
    for (const std::string& s : n.recentServers) recent += (recent.empty() ? "" : ";") + s;
    Write(path, L"recent_servers", recent);
}

std::map<std::string, PlayerSetup> ReadLoadouts(const std::wstring& path)
{
    std::map<std::string, PlayerSetup> loadouts;
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(L"loadouts", buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
    // "KEN=3 5 0 2 1 0\0CMY=...\0\0"
    for (size_t at = 0; at < n && buf[at];) {
        const std::wstring entry(buf.data() + at);
        at += entry.size() + 1;
        const size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        int costume = 0, color = 1, comment = 0, vskill = 1, vtrigger = 1, variant = 0;
        if (std::swscanf(entry.c_str() + eq + 1, L"%d %d %d %d %d %d", &costume, &color, &comment, &vskill, &vtrigger,
                         &variant) != 6) {
            continue;
        }
        const std::string code = Ascii(entry.substr(0, eq));
        PlayerSetup p;
        p.character = PackCode(code.c_str());
        p.costume = static_cast<uint16_t>(costume);
        p.color = static_cast<uint8_t>(color - 1);
        p.comment = static_cast<uint8_t>(comment);
        p.vskill = static_cast<uint8_t>(vskill - 1);
        p.vtrigger = static_cast<uint8_t>(vtrigger - 1);
        p.variation = static_cast<uint8_t>(variant);
        loadouts[code] = p;
    }
    return loadouts;
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
    n.name = ReadUtf8(path, "weyve", "name"); // UTF-8 (Notepad's default)
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
