#include "mod/control.h"
#include "mod/battle_driver.h"
#include "mod/files.h"
#include "mod/pacing.h"
#include "mod/updater.h"
#include "mod/flow.h"

#include "mod/lifecycle.h"
#include "mod/stack.h"
#include "mod/battle_setup.h"
#include "mod/boot.h"
#include "mod/weyve_service.h"
#include "mod/sfv_game.h"
#include "mod/status.h"
#include "mod/watch.h"
#include "mod/log.h"
#include "mod/memscan.h"
#include "mod/ue4.h"
#include "mod/netplay_driver.h"
#include "mod/outside_probe.h"
#include "mod/overlay.h"
#include "mod/replay_driver.h"
#include "mod/tracer.h"

#include <MinHook.h>
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace sfv::mod {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct Step {
    enum Kind { Press, Wait, Connect, Disconnect, Game, Setup, ShowSetup, ClearSetup, Scan, Rescan, ScanShow, ScanSmall, ScanDelta, ScanRecord, Trace, TraceReport, Net } kind;
    std::string line;
    MatchSetup setup;
    std::vector<uint32_t> rvas;
    int user = 0;
    WORD buttons = 0;
    BYTE lt = 0, rt = 0;
    int holdMs = 0;
    int gapMs = 0;
    GameRequest request;
};

struct VirtualPad {
    std::atomic<bool> connected{false};
    std::atomic<WORD> buttons{0};
    std::atomic<BYTE> lt{0}, rt{0};
    std::atomic<DWORD> packet{0};
    std::atomic<bool> scripted{false};
    std::atomic<ULONGLONG> scriptedAt{0}; // scripted state expires: a dead driver must not keep pressing
    std::atomic<WORD> scriptedButtons{0};
    std::atomic<BYTE> scriptedRt{0};
};

XInputGetStateFn g_origGetState = nullptr;
std::atomic<bool> g_blockPause{false};
std::atomic<uint16_t> g_uiButtons{0};      // PressUiButtons
std::atomic<uint64_t> g_uiButtonsUntil{0};
using HwndFn = HWND(WINAPI*)();
HwndFn g_origForeground = nullptr;
HwndFn g_origActive = nullptr;
HwndFn g_origFocus = nullptr;
std::atomic<HWND> g_gameWindow{nullptr};
// always_active was asked for but the window did not exist yet (cold start).
std::atomic<bool> g_activePending{false};
std::atomic<uint16_t> g_realStartBack[XUSER_MAX_COUNT] = {};
void KeepWindowActive();
void SubclassWindow();

HWND FindGameWindow()
{
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM out) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t cls[64];
            GetClassNameW(w, cls, 64);
            if (pid == GetCurrentProcessId() && lstrcmpW(cls, L"UnrealWindow") == 0 && IsWindowVisible(w)) {
                *reinterpret_cast<HWND*>(out) = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND WINAPI ForegroundDetour()
{
    HookGuard guard;
    HWND w = g_gameWindow;
    return w ? w : g_origForeground();
}
HWND WINAPI ActiveDetour()
{
    HookGuard guard;
    HWND w = g_gameWindow;
    return w ? w : g_origActive();
}
HWND WINAPI FocusDetour()
{
    HookGuard guard;
    HWND w = g_gameWindow;
    return w ? w : g_origFocus();
}
uintptr_t g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
VirtualPad g_pads[XUSER_MAX_COUNT];
std::atomic<uint64_t> g_polls[XUSER_MAX_COUNT];
std::mutex g_mutex;
std::deque<GameRequest> g_gameRequests;

std::atomic<bool> g_isolatePads{false};
std::atomic<PackedInput> g_padInput[XUSER_MAX_COUNT];

// The game's input manager (rva 0x3A750C0; its +8 holds two pads of 0x200
// bytes) maps the keyboard and the pads, with the player's button config,
// once per frame (rva 0x4DC7D0). Per pad: +0x4 the pad keys down (bit k =
// EKWKeyConfigPadKeyId k: X 0x10, Y 0x20, A 0x40, ... LB 0x100), +0x8
// pressed this frame, +0xC released, +0x14 the value the battle reads (rva
// 0x4CED60 via 0x4CEDF0; the InputUnit's values come from it, rva 0x443D20):
// the keys after the player's Button Settings, each function as the default
// layout's keys, shortcuts expanded (3P on LB: X+Y+RB). +0x20 / +0x24 the
// stick. The local input is +0x14, as the battle reads it offline: +0x4
// skipped the Button Settings, so shortcut keys and custom layouts (keyboard
// players have both) came out wrong online. The local player plays with pad 0
// (keyboard included). In a netplay match the battle must see none of it,
// only the injected inputs: the game read local input past the InputUnit and
// each machine saw its own player's pad (netplay-testing.md, finding 7), so
// the manager's pads are cleared right after each update, and put back just
// before the next one (the update derives pressed and held from them).
constexpr uint32_t kInputManager = 0x3A750C0;
constexpr uint32_t kInputManagerUpdate = 0x4DC7D0;
constexpr uint8_t kInputManagerUpdateBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55,
                                                0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr uint32_t kPadStride = 0x200;
constexpr uint32_t kPadDown = 0x04;
constexpr uint32_t kPadBattle = 0x14;
constexpr uint32_t kPadStateBegin = 0x04, kPadStateEnd = 0x28;
using InputUpdateFn = uint64_t (*)(void* manager, float deltaSeconds);
InputUpdateFn g_origInputUpdate = nullptr;
std::atomic<PackedInput> g_mappedInput{0};
std::atomic<int> g_mappedLog{0}; // research (`mappedlog <updates>`): log the mapped value on change
// The two pads' state as the last update left it, before the clearing (game thread only).
uint8_t g_padsKept[2][kPadStateEnd - kPadStateBegin] = {};
bool g_padsCleared = false;

uint64_t InputUpdateDetour(void* self, float deltaSeconds)
{
    HookGuard guard;
    // During a netplay match the frame's wait goes here, before the pads are
    // read, so it adds no input lag (pacing.h).
    PaceFrame();
    auto* pads = *reinterpret_cast<uint8_t* const*>(g_imageBase + kInputManager + 8);
    const bool padsOk = pads && IsReadable(pads, 2 * kPadStride);
    // Put back what the clearing took, so this update sees the last one's state.
    if (padsOk && g_padsCleared) {
        for (int pad = 0; pad < 2; pad++) std::memcpy(pads + pad * kPadStride + kPadStateBegin, g_padsKept[pad], sizeof(g_padsKept[pad]));
    }
    g_padsCleared = false;
    const uint64_t result = g_origInputUpdate(self, deltaSeconds);
    if (padsOk) {
        const PackedInput local = *reinterpret_cast<const uint32_t*>(pads + kPadBattle);
        if (g_mappedLog > 0) {
            g_mappedLog--;
            if (local != g_mappedInput) {
                Log("mappedlog: pad 0 %08X, keys %08X (pad 1 %08X)", local, *reinterpret_cast<const uint32_t*>(pads + kPadDown),
                    *reinterpret_cast<const uint32_t*>(pads + kPadStride + kPadBattle));
            }
        }
        g_mappedInput = local;
        if (g_isolatePads) {
            for (int pad = 0; pad < 2; pad++) {
                std::memcpy(g_padsKept[pad], pads + pad * kPadStride + kPadStateBegin, sizeof(g_padsKept[pad]));
                std::memset(pads + pad * kPadStride + kPadStateBegin, 0, kPadStateEnd - kPadStateBegin);
            }
            g_padsCleared = true;
        }
    }
    return result;
}

// The game's input value for a pad state, with the default button layout
// (type A: X LP, Y MP, RB HP, A LK, B MK, RT HK), d-pad or left stick.
PackedInput ToGameInput(const XINPUT_GAMEPAD& g)
{
    constexpr SHORT kStick = 16000;
    PackedInput in = 0;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > kStick) in |= kUp;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -kStick) in |= kDown;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -kStick) in |= kLeft;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > kStick) in |= kRight;
    if (g.wButtons & XINPUT_GAMEPAD_X) in |= kLP;
    if (g.wButtons & XINPUT_GAMEPAD_Y) in |= kMP;
    if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) in |= kHP;
    if (g.wButtons & XINPUT_GAMEPAD_A) in |= kLK;
    if (g.wButtons & XINPUT_GAMEPAD_B) in |= kMK;
    if (g.bRightTrigger > 64) in |= kHK;
    if (g.wButtons & XINPUT_GAMEPAD_START) in |= kStart;
    return in;
}

DWORD ComposeState(DWORD user, XINPUT_STATE* state)
{
    DWORD result = g_origGetState(user, state);
    if (user >= XUSER_MAX_COUNT) return result;
    g_polls[user]++;
    // Start and Back as the player pressed them (the pause filter below
    // removes them), for the overlay's Back + Start.
    g_realStartBack[user] = result == ERROR_SUCCESS
                                ? static_cast<uint16_t>(state->Gamepad.wButtons & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK))
                                : 0;

    // Real pads too: a pause on one side of a netplay match would desync it.
    if (g_blockPause && result == ERROR_SUCCESS) state->Gamepad.wButtons &= ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK);

    VirtualPad& pad = g_pads[user];
    if (!pad.connected) return result;
    if (result != ERROR_SUCCESS) std::memset(state, 0, sizeof(*state));
    state->dwPacketNumber = pad.packet.load() + (result == ERROR_SUCCESS ? state->dwPacketNumber : 0);
    if (pad.scripted && GetTickCount64() - pad.scriptedAt > 500) pad.scripted = false;
    if (pad.scripted) {
        state->Gamepad = {};
        // Pad steps (testing) still add their buttons, as on an unscripted pad.
        state->Gamepad.wButtons =
            (pad.scriptedButtons.load() & (g_blockPause ? ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK) : 0xFFFF)) |
            pad.buttons.load();
        state->Gamepad.bRightTrigger = pad.scriptedRt.load();
        return ERROR_SUCCESS;
    }
    state->Gamepad.wButtons |= pad.buttons.load();
    state->Gamepad.bLeftTrigger = std::max(state->Gamepad.bLeftTrigger, pad.lt.load());
    state->Gamepad.bRightTrigger = std::max(state->Gamepad.bRightTrigger, pad.rt.load());
    return ERROR_SUCCESS;
}

DWORD WINAPI GetStateDetour(DWORD user, XINPUT_STATE* state)
{
    HookGuard guard;
    // The engine polls the pads on the game thread once per frame.
    if (user == 0) PumpGameThread();
    const DWORD result = ComposeState(user, state);
    if (user >= XUSER_MAX_COUNT) return result;
    // The overlay sees the pads as composed (real and virtual), with the real
    // Start and Back.
    if (result == ERROR_SUCCESS) {
        XINPUT_GAMEPAD pad = state->Gamepad;
        pad.wButtons |= g_realStartBack[user];
        OverlayPad(user, pad);
    }
    // The overlay's panel (or the post-match vote) is open: the game (and a
    // netplay match) gets neutral pads.
    if (OverlayTakesInput() && result == ERROR_SUCCESS) state->Gamepad = {};
    // With the real Start (the pause filter took it out of the state): a
    // netplay match passes it on only when both players pressed it.
    XINPUT_GAMEPAD forMatch = state->Gamepad;
    if (!OverlayTakesInput()) forMatch.wButtons |= g_realStartBack[user] & XINPUT_GAMEPAD_START;
    g_padInput[user] = result == ERROR_SUCCESS ? ToGameInput(forMatch) : 0;
    if (user == 0 && result == ERROR_SUCCESS && GetTickCount64() < g_uiButtonsUntil) {
        state->Gamepad.wButtons |= g_uiButtons;
        static uint64_t loggedFor = 0;
        if (loggedFor != g_uiButtonsUntil) Log("control: buttons %04x on the game's pad input", g_uiButtons.load());
        loggedFor = g_uiButtonsUntil;
    }
    return result;
}

// host [random] [costumes] | join <code> | spectate <code> | leave | ready [0|1]
// | choose rematch|select | netsim <latency ms> <jitter ms> <loss %> | mash <seed>
// | dumps <every N frames> [max] (state dumps in match recordings)
// | recordings full|compact (region hashes for every frame, or only around desyncs)
// | server [host:port] (the rooms server for the next host/join, this game only)
// | packlogs (the SFVR folder's logs and latest matches into a zip to share)
// | update [check [url] | install] (the updater: check the site, install what it offers)
// | pick <code costume color comment vskill vtrigger> | stage <n> | netstatus
// | name [text] (the name test sessions use; none: back to "Player <instance + 1>")
// | delay <frames> (this player's input delay for the next host/join)
// | replay <file> [stress <distance>] [watch <from> <to> <region>] | replay stop (plays a .sfvr back in the game, replay_driver.h)
// Host and join from commands are ready at once (scripts drive the lobby),
// and their player is named "Player <instance + 1>" (the panel's or Steam's
// name stays for sessions started by hand); `name` sets another, e.g. the
// second PC's instance 0 in a two-machine test.
bool ParseButtons(const std::string& spec, Step& step);

std::string g_testName; // from `name`; empty: "Player <instance + 1>"

std::string TestName()
{
    return g_testName.empty() ? "Player " + std::to_string(StatusInstance() + 1) : g_testName;
}

void RunNetCommand(const std::string& line)
{
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    NetplayOptions& o = NetOptions();
    if (cmd == "host") {
        // host [random [costumes]] [manual]: manual is not ready by itself in
        // the lobby (to see the lobby's windows in a test).
        std::string word;
        o.randomSetup = false;
        o.randomCostumes = false;
        bool manual = false;
        while (ss >> word) {
            if (word == "random") o.randomSetup = true;
            if (word == "costumes") o.randomCostumes = true;
            if (word == "manual") manual = true;
        }
        NetplayOptions command = o;
        command.autoReady = !manual;
        command.name = TestName();
        NetHost(command);
    }
    else if (cmd == "join" || cmd == "spectate") {
        std::string code, word;
        ss >> code >> word;
        NetplayOptions command = o;
        command.autoReady = word != "manual";
        command.name = TestName();
        NetJoin(code, command, cmd == "spectate");
    }
    else if (cmd == "delay") {
        // delay <frames>: this player's input delay for the next host/join.
        int frames = 2;
        ss >> frames;
        o.inputDelay = static_cast<uint8_t>(std::clamp(frames, 0, 10));
        Log("netplay: input delay %u for the next session", o.inputDelay);
    }
    else if (cmd == "name") {
        std::getline(ss >> std::ws, g_testName);
        Log("netplay: test sessions play as %s", TestName().c_str());
    }
    else if (cmd == "update") {
        // update [check [url]] | update install: the updater (updater.h).
        std::string what, url;
        ss >> what >> url;
        if (what == "check") {
            if (!url.empty()) SetUpdateUrl(url);
            UpdateCheck();
        }
        else if (what == "install") UpdateInstall();
        const UpdateView v = UpdateViewNow();
        Log("update: state %d, this build %s, site's %s, %llu of %llu bytes%s%s", static_cast<int>(v.state), v.current.c_str(),
            v.latest.empty() ? "-" : v.latest.c_str(), static_cast<unsigned long long>(v.done),
            static_cast<unsigned long long>(v.total), v.message.empty() ? "" : ": ", v.message.c_str());
    }
    else if (cmd == "packlogs") {
        // packlogs: what Settings, Logs, "Pack logs to share" does (files.h).
        std::string error;
        if (PackLogsToShare(&error).empty()) Log("files: cannot pack the logs: %s", error.c_str());
    }
    else if (cmd == "server") {
        // server [host:port [also port]]: the rooms server the next host/join
        // uses, in this game only (the panel keeps its own choice). None:
        // back to the default. The second port is tried at the same time
        // (testing the public server's 443 fallback).
        std::string address;
        int alsoPort = 0;
        ss >> address >> alsoPort;
        o.server = !address.empty() ? address : !o.savedServer.empty() ? o.savedServer : kDefaultServer;
        o.serverAlsoPort = static_cast<uint16_t>(std::clamp(alsoPort, 0, 65535));
        Log("netplay: rooms server %s for the next sessions", o.server.c_str());
    }
    else if (cmd == "transport") {
        // transport udp|weyve: for the next host/join (testing both paths).
        std::string which;
        ss >> which;
        o.udp = which == "udp";
        Log("netplay: transport %s", o.udp ? "UDP" : ("Weyvelength " + o.server).c_str());
    }
    else if (cmd == "ready") {
        int on = 1;
        ss >> on;
        NetSetReady(on != 0);
    }
    else if (cmd == "choose") {
        std::string what;
        ss >> what;
        NetChoose(what == "rematch" ? lobby::Choice::Rematch : what == "select" ? lobby::Choice::CharacterSelect : lobby::Choice::None);
    }
    else if (cmd == "leave") {
        NetLeave();
    }
    else if (cmd == "park") {
        NetPark();
    }
    else if (cmd == "members") {
        // members <hex address> [bytes]: names the UObjects its fields point at.
        unsigned long long address = 0;
        int bytes = 0x400;
        ss >> std::hex >> address >> std::dec >> bytes;
        Log("members of %s:", ue4::Describe(reinterpret_cast<const void*>(address)).c_str());
        for (int off = 0; off < bytes; off += 8) {
            uint64_t v = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + off), &v, 8, &got)) break;
            if (v < 0x10000000000ull || v >= 0x800000000000ull) continue;
            const std::string d = ue4::Describe(reinterpret_cast<const void*>(v));
            if (d.find('?') == std::string::npos && d.size() > 3 && d.front() != ' ') Log("  +0x%x: %s (%llx)", off, d.c_str(), static_cast<unsigned long long>(v));
        }
    }
    else if (cmd == "classes") {
        // classes <text>: classes whose name contains `text`, with how many
        // live objects each has (diagnostics).
        std::string text;
        ss >> text;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        std::vector<const void*> classes;
        ue4::ForEachObject([&](const void* object) {
            const std::string kind = ue4::ObjectName(ue4::ClassOf(object));
            if ((kind == "Class" || kind == "BlueprintGeneratedClass") && ue4::ObjectName(object).find(text) != std::string::npos &&
                classes.size() < 60) {
                classes.push_back(object);
            }
        });
        for (const void* cls : classes) {
            int count = 0;
            const void* first = nullptr;
            ue4::ForEachObject([&](const void* object) {
                for (const void* c = ue4::ClassOf(object); c; c = ue4::SuperOf(c)) {
                    if (c != cls) continue;
                    if (!count++) first = object;
                    break;
                }
            });
            Log("class %s (%p): %d objects, first %s (%p)", ue4::ObjectName(cls).c_str(), cls, count,
                ue4::Describe(first).c_str(), first);
        }
    }
    else if (cmd == "packages") {
        // packages mark: remembers the loaded packages. packages new: logs
        // the packages loaded since (with how many objects each holds) and
        // those gone since (diagnostics: what a battle load brings in).
        std::string what;
        ss >> what;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        std::map<std::string, int> packages; // name -> objects inside
        ue4::ForEachObject([&](const void* object) {
            const void* root = object;
            while (const void* outer = ue4::OuterOf(root)) root = outer;
            if (ue4::ObjectName(ue4::ClassOf(root)) != "Package") return;
            packages[ue4::ObjectName(root)]++;
        });
        static std::map<std::string, int> marked;
        if (what == "mark") {
            marked = packages;
            Log("packages: %zu marked", marked.size());
            return;
        }
        size_t added = 0, objects = 0;
        for (const auto& [name, count] : packages) {
            if (marked.count(name)) continue;
            Log("package+ %s %d", name.c_str(), count);
            added++;
            objects += count;
        }
        size_t gone = 0;
        for (const auto& [name, count] : marked) {
            if (packages.count(name)) continue;
            Log("package- %s %d", name.c_str(), count);
            gone++;
        }
        Log("packages: %zu now, %zu new (%zu objects), %zu gone since the mark", packages.size(), added, objects, gone);
    }
    else if (cmd == "preload") {
        // preload <code> [costume]: into the game's character cache.
        std::string code;
        int costume = 0;
        ss >> code >> costume;
        PreloadCharacter(code, costume);
    }
    else if (cmd == "versions") {
        LogCharaVersions();
    }
    else if (cmd == "uistart") {
        // uistart [ms] [buttons]: testing, buttons (default start) on the
        // game's own pad input (PressUiButtons).
        int ms = 150;
        std::string buttons = "start";
        ss >> ms >> buttons;
        Step parsed;
        if (ParseButtons(buttons, parsed)) PressUiButtons(parsed.buttons, ms);
    }
    else if (cmd == "uipads") {
        // uipads <0|1>: testing, 1 lets the game see the local pads (with
        // Start and Back) during a netplay match, 0 isolates them again.
        int on = 0;
        ss >> on;
        SetPadIsolation(!on);
        SetBlockPause(!on);
        Log("control: game sees the local pads during a match: %d", on);
    }
    else if (cmd == "mappedlog") {
        int updates = 0;
        ss >> updates;
        g_mappedLog = updates;
        Log("mappedlog: the input manager's pad 0 for the next %d updates, on change", updates);
    }
    else if (cmd == "inputlog") {
        int ticks = 0;
        ss >> ticks;
        LogGameInputs(ticks);
    }
    else if (cmd == "drawlog") {
        int count = 0;
        ss >> count;
        LogRandomDraws(count);
    }
    else if (cmd == "charcache") {
        LogCharaCache();
    }
    else if (cmd == "datatable") {
        std::string name, filter;
        int limit = 200;
        ss >> name >> filter >> limit;
        if (filter == "-") filter.clear();
        LogDataTable(name, filter, limit);
    }
    else if (cmd == "variants") {
        LogAllVariants();
    }
    else if (cmd == "uniqueparam") {
        int player = 0, index = 0, value = -1;
        ss >> player >> index >> value;
        SetUniqueParamOverride(player, index, value);
        Log("uniqueparam: P%d [%d] = %d", player + 1, index, value);
    }
    else if (cmd == "vsync") {
        std::string what;
        ss >> what;
        if (what == "on" || what == "off") SetVsync(what == "on");
        Log("overlay: %s", VsyncInfo().c_str());
    }
    else if (cmd == "relayonly") {
        // relayonly on|off: the next sessions' links go only through TURN (testing).
        std::string what;
        ss >> what;
        WeyveService::SetRelayOnly(what != "off");
    }
    else if (cmd == "freeplay") {
        std::string what;
        ss >> what;
        SetFreePlayGuard(what);
    }
    else if (cmd == "presets") {
        std::string code;
        ss >> code;
        LogCostumePresets(code);
    }
    else if (cmd == "costumes") {
        std::string code;
        ss >> code;
        LogCharaCostumes(code);
    }
    else if (cmd == "charas") {
        std::string list;
        for (int i = 0; i < CharaCount(); i++) {
            list += " " + CharaCode(i) + "=" + CharaName(i) + "(" + std::to_string(CharaCostumeCount(i)) + ")";
        }
        Log("characters (%d):%s", CharaCount(), list.c_str());
    }
    else if (cmd == "watchregion") {
        // watchregion <region name> <hex offset> [bytes]: for the next battle.
        std::string name;
        size_t offset = 0;
        int bytes = 8;
        ss >> name >> std::hex >> offset >> std::dec >> bytes;
        WatchRegionOfNextBattle(name, offset, bytes);
    }
    else if (cmd == "overlay") {
        // overlay show|hide|toggle|preview <words>
        std::string what;
        ss >> what;
        if (what == "show") SetOverlayVisible(true);
        else if (what == "hide") SetOverlayVisible(false);
        else if (what == "preview") {
            std::string rest;
            std::getline(ss, rest);
            PreviewOverlay(rest);
        }
        else ToggleOverlay();
    }
    else if (cmd == "menu") {
        ReturnToMainMenu(1);
    }
    else if (cmd == "relocate") {
        // Finds the object array again (diagnostics).
        const auto engine = *reinterpret_cast<uint8_t* const*>(g_imageBase + 0x3D878C8);
        ue4::LocateObjectsFrom(engine ? *reinterpret_cast<void* const*>(engine + 0x1E0) : nullptr);
    }
    else if (cmd == "scene") {
        // scene <StateClass> [init message object name]
        std::string name, param;
        ss >> name >> param;
        ChangeScene(name, param);
    }
    else if (cmd == "named") {
        // named <object name>: every object with that name, with its outer.
        std::string name;
        ss >> name;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        int shown = 0;
        ue4::ForEachObject([&](const void* object) {
            if (shown >= 20 || ue4::ObjectName(object) != name) return;
            shown++;
            Log("named %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::Describe(ue4::OuterOf(object)).c_str());
        });
    }
    else if (cmd == "find") {
        // find <text>: objects whose name contains `text` (case-insensitive), up to 60.
        std::string text;
        ss >> text;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        auto lower = [](std::string v) {
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return v;
        };
        const std::string needle = lower(text);
        int shown = 0;
        ue4::ForEachObject([&](const void* object) {
            if (shown >= 60 || lower(ue4::ObjectName(object)).find(needle) == std::string::npos) return;
            shown++;
            Log("find %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::ObjectName(ue4::OuterOf(object)).c_str());
        });
        Log("find %s: %d shown", text.c_str(), shown);
    }
    else if (cmd == "classinfo") {
        // classinfo <class name>: its fields (UField chain from +0x38, next at
        // +0x28) with the raw words that hold a property's offset and size.
        std::string name;
        ss >> name;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        const auto* cls = static_cast<const uint8_t*>(ue4::FindObject(name, "Class"));
        if (!cls) cls = static_cast<const uint8_t*>(ue4::FindObject(name, "BlueprintGeneratedClass"));
        Log("classinfo %s: %p, super %s, size 0x%zx", name.c_str(), cls, cls ? ue4::ObjectName(ue4::SuperOf(cls)).c_str() : "",
            cls ? static_cast<size_t>(*reinterpret_cast<const int32_t*>(cls + 0x40)) : 0);
        // Fields of a struct: name, then the raw words after UField (property
        // offset at +0x4C).
        auto fields = [](const uint8_t* owner, const char* indent) {
            const uint8_t* f = owner ? *reinterpret_cast<const uint8_t* const*>(owner + 0x38) : nullptr;
            for (int i = 0; f && i < 200 && IsReadable(f, 0x80); i++) {
                const auto* w = reinterpret_cast<const int32_t*>(f + 0x30);
                Log("%s%s: %x %x %x %x %x %x %x %x", indent, ue4::Describe(f).c_str(), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                    w[7]);
                // An array of structs: ArrayProperty +0x70 inner property,
                // StructProperty +0x70 its struct (PropertiesSize at +0x40).
                // A struct field: the same, from the field itself.
                const std::string fieldClass = ue4::ObjectName(ue4::ClassOf(f));
                if (fieldClass == "ArrayProperty" || fieldClass == "StructProperty") {
                    const auto* inner = fieldClass == "StructProperty" ? f : *reinterpret_cast<const uint8_t* const*>(f + 0x70);
                    if (inner && ue4::ObjectName(ue4::ClassOf(inner)) == "StructProperty") {
                        const auto* st = *reinterpret_cast<const uint8_t* const*>(inner + 0x70);
                        if (st && IsReadable(st, 0x48)) {
                            Log("%s  of %s, size 0x%x:", indent, ue4::Describe(st).c_str(), *reinterpret_cast<const int32_t*>(st + 0x40));
                            const uint8_t* g = *reinterpret_cast<const uint8_t* const*>(st + 0x38);
                            for (int j = 0; g && j < 100 && IsReadable(g, 0x50); j++) {
                                Log("%s    %s at +0x%x", indent, ue4::Describe(g).c_str(), *reinterpret_cast<const int32_t*>(g + 0x4C));
                                g = *reinterpret_cast<const uint8_t* const*>(g + 0x28);
                            }
                        }
                    }
                }
                f = *reinterpret_cast<const uint8_t* const*>(f + 0x28);
            }
        };
        fields(cls, "  ");
    }
    else if (cmd == "poke") {
        // poke <hex address> <hex value> [bytes 1|4|8]: testing only.
        unsigned long long address = 0, value = 0;
        int bytes = 4;
        ss >> std::hex >> address >> value >> std::dec >> bytes;
        SIZE_T done = 0;
        const bool ok = (bytes == 1 || bytes == 4 || bytes == 8) &&
                        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, bytes, &done);
        Log("poke %llx = %llx (%d bytes): %s", address, value, bytes, ok ? "ok" : "failed");
    }
    else if (cmd == "findbytes") {
        // findbytes <hex bytes, e.g. 0a0b0c>: where they are in writable memory (research).
        std::string hex;
        ss >> hex;
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        FindBytes(bytes);
    }
    else if (cmd == "owner") {
        // owner <hex address>: the UObject whose memory holds it, and the offset (research).
        unsigned long long address = 0;
        ss >> std::hex >> address;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        int found = 0;
        ue4::ForEachObject([&](const void* object) {
            const auto start = reinterpret_cast<uintptr_t>(object);
            const size_t size = ue4::ObjectSize(object);
            if (address < start || address >= start + size || size == 0) return;
            Log("owner %llx: %s +0x%llx (size 0x%zx)", address, ue4::Describe(object).c_str(),
                static_cast<unsigned long long>(address - start), size);
            found++;
        });
        if (!found) Log("owner %llx: no UObject holds it", address);
    }
    else if (cmd == "enuminfo") {
        // enuminfo <enum name>: its names and values (UEnum::Names at +0x40,
        // FNames in value order (this engine version); research).
        std::string name;
        ss >> name;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        const auto* e = static_cast<const uint8_t*>(ue4::FindObject(name, "Enum"));
        if (!e || !IsReadable(e, 0x50)) {
            Log("enuminfo: no enum %s", name.c_str());
            return;
        }
        const auto* pairs = *reinterpret_cast<const uint8_t* const*>(e + 0x40);
        const int32_t count = *reinterpret_cast<const int32_t*>(e + 0x48);
        if (!pairs || count <= 0 || count > 512 || !IsReadable(pairs, static_cast<size_t>(count) * 8)) {
            Log("enuminfo %s: unexpected layout (%p, %d)", name.c_str(), pairs, count);
            return;
        }
        for (int32_t i = 0; i < count; i++) {
            const int32_t index = *reinterpret_cast<const int32_t*>(pairs + i * 8);
            Log("enuminfo %s: %d %s", name.c_str(), i, ue4::NameText(index).c_str());
        }
    }
    else if (cmd == "keyconfigs") {
        // keyconfigs: the button settings (KWKeyConfigs, KWUserProfileDetails
        // +0xB0: 15 bytes Data, then TypeData) of every user profile (research).
        if (!ue4::EnsureObjects(g_imageBase)) return;
        ue4::ForEachObject([](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) != "KWUserProfileDetails") return;
            const auto* p = static_cast<const uint8_t*>(object);
            if (!IsReadable(p, 0xC0)) return;
            char hex[64] = {};
            for (int i = 0; i < 16; i++) std::snprintf(hex + i * 2, 3, "%02x", p[0xB0 + i]);
            Log("keyconfigs: %s (%p) main %d: %s", ue4::ObjectName(object).c_str(), object, p[0x28] & 1, hex);
        });
    }
    else if (cmd == "findptr") {
        unsigned long long v = 0;
        ss >> std::hex >> v;
        FindPointer(v);
    }
    else if (cmd == "replay") {
        // replay <file in SFVR\matches (or reports\) or a full path> [stress <distance>]
        // ... [watch <from> <to> <region>]
        std::string file, word, region;
        int stress = 0, from = -1, to = -1;
        ss >> file;
        while (ss >> word) {
            if (word == "stress") ss >> stress;
            if (word == "watch") ss >> from >> to >> region;
        }
        RequestReplay(file, stress, from, to, region);
    }
    else if (cmd == "regions") {
        size_t size = 0;
        ss >> size;
        LogRegionsOfSize(size);
    }
    else if (cmd == "phases") {
        LogPhaseMachine();
    }
    else if (cmd == "tasklist") {
        int calls = 3000;
        ss >> calls;
        TaskCensus(calls);
    }
    else if (cmd == "outsidetest") {
        int ticks = 8;
        std::string mode;
        ss >> ticks >> mode;
        if (ue4::EnsureObjects(g_imageBase)) RequestOutsideProbe(nullptr, ticks, mode == "all");
        else Log("outside: object array not found");
    }
    else if (cmd == "recdiff") {
        // The int32 fields that differ between the P1 and P2 battle records.
        uint8_t* data = BattleData();
        if (!data) {
            Log("recdiff: no battle data");
        }
        else {
            for (uint32_t off = 0; off < battledata::kRecordStride; off += 4) {
                const int32_t a = *reinterpret_cast<int32_t*>(data + off);
                const int32_t b = *reinterpret_cast<int32_t*>(data + battledata::kRecordStride + off);
                if (a != b) Log("recdiff +0x%03x: P1 %d (%08x) P2 %d (%08x)", off, a, a, b, b);
            }
        }
    }
    else if (cmd == "nextstage") {
        std::string code;
        ss >> code;
        SetPendingStage(code);
        Log("next stage: %s", code.empty() ? "(game's choice)" : code.c_str());
    }
    else if (cmd == "stages") {
        LogStageTable();
    }
    else if (cmd == "worldctx") {
        LogWorldContext();
    }
    else if (cmd == "battlenow") {
        JumpToBattle();
    }
    else if (cmd == "netsim") {
        double loss = 0;
        ss >> o.conditions.latencyMs >> o.conditions.jitterMs >> loss;
        o.conditions.loss = loss / 100.0;
        NetSetConditions(o.conditions);
    }
    else if (cmd == "mash") {
        o.mashFrom = 0;
        ss >> o.mashSeed >> o.mashFrom;
        Log("netplay: local player mashes with seed %llu", static_cast<unsigned long long>(o.mashSeed));
    }
    else if (cmd == "dumps") {
        ss >> o.dumpEvery >> o.maxDumps;
        Log("netplay: recordings dump the state every %d frames, at most %d times", o.dumpEvery, o.maxDumps);
    }
    else if (cmd == "recordings") {
        // recordings full|compact: which frames' region hashes the next saved recordings keep.
        std::string what;
        ss >> what;
        if (what == "full" || what == "compact") o.fullRecordings = what == "full";
        Log("netplay: recordings are %s", o.fullRecordings ? "full (every frame's region hashes)" : "compact (region hashes around desyncs)");
    }
    else if (cmd == "pick") {
        std::string code;
        int costume = 0, color = 1, comment = 0, vskill = 1, vtrigger = 1;
        ss >> code >> costume >> color >> comment >> vskill >> vtrigger;
        o.pick.variation = 0;
        if (const size_t colon = code.find(':'); colon != std::string::npos) {
            o.pick.variation = static_cast<uint8_t>(std::atoi(code.c_str() + colon + 1));
            code.resize(colon);
        }
        o.hasPick = !code.empty();
        o.pick.character = PackCode(code.c_str());
        o.pick.costume = static_cast<uint16_t>(costume);
        o.pick.color = static_cast<uint8_t>(color - 1);
        o.pick.comment = static_cast<uint8_t>(comment);
        o.pick.vskill = static_cast<uint8_t>(vskill - 1);
        o.pick.vtrigger = static_cast<uint8_t>(vtrigger - 1);
        NetSetPick(o.hasPick, o.pick);
    }
    else if (cmd == "stage") {
        // stage <code> | stage <table index> | stage (keep)
        std::string v;
        ss >> v;
        o.stageCode.clear();
        o.stage = kKeepStage;
        if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) o.stage = static_cast<uint16_t>(std::stoi(v));
        else o.stageCode = v;
        // In a session, the host's stage for the next match (as the panel sets it).
        if (!o.stageCode.empty()) NetSetStage(o.stageCode);
    }
    else if (cmd == "netstatus") {
        Log("netplay status: %s", NetStatus().c_str());
    }
    else if (cmd == "battle") {
        Log("battle: %s | rendering %.1f fps", BattleInfo().c_str(), RenderFps());
    }
    else if (cmd == "peek") {
        // peek <hex address> [qwords]: logs memory, with image addresses as RVAs.
        unsigned long long address = 0;
        int count = 4;
        ss >> std::hex >> address >> std::dec >> count;
        for (int i = 0; i < count && i < 64; i++) {
            uint64_t v = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 8 * i), &v, 8, &got)) break;
            const bool inImage = v >= g_imageBase && v < g_imageBase + 0x5000000;
            Log("peek %llx: %016llx%s", address + 8 * i, static_cast<unsigned long long>(v),
                inImage ? (" (rva 0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v - g_imageBase)); return std::string(b); }() + ")").c_str() : "");
        }
    }
    else if (cmd == "setstage") {
        int stage = 0;
        ss >> stage;
        uint8_t* data = BattleData();
        if (data) *reinterpret_cast<int32_t*>(data + battledata::kStage) = stage;
        Log("setstage %d: %s", stage, data ? "written" : "no battle data now");
    }
}

constexpr int kBothPads = -2; // Step::user: pads 0 and 1 together

bool ParseButtons(const std::string& spec, Step& step)
{
    std::stringstream ss(spec);
    std::string b;
    while (std::getline(ss, b, '+')) {
        if (b == "up") step.buttons |= XINPUT_GAMEPAD_DPAD_UP;
        else if (b == "down") step.buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        else if (b == "left") step.buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (b == "right") step.buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        else if (b == "start") step.buttons |= XINPUT_GAMEPAD_START;
        else if (b == "back") step.buttons |= XINPUT_GAMEPAD_BACK;
        else if (b == "lb") step.buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        else if (b == "rb") step.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else if (b == "a") step.buttons |= XINPUT_GAMEPAD_A;
        else if (b == "b") step.buttons |= XINPUT_GAMEPAD_B;
        else if (b == "x") step.buttons |= XINPUT_GAMEPAD_X;
        else if (b == "y") step.buttons |= XINPUT_GAMEPAD_Y;
        else if (b == "lt") step.lt = 255;
        else if (b == "rt") step.rt = 255;
        else if (b != "none") return false;
    }
    return true;
}

bool Parse(const std::string& line, Step& step)
{
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "pad") {
        // pad <user|both> <buttons> [hold ms, -1: keep] [gap ms]; both: pads 0 and 1 at once.
        std::string user, buttons;
        step.kind = Step::Press;
        step.gapMs = 50;
        ss >> user >> buttons >> step.holdMs >> step.gapMs;
        step.user = user == "both" ? kBothPads : std::atoi(user.c_str());
        return (step.user == kBothPads || (step.user >= 0 && step.user < XUSER_MAX_COUNT)) && ParseButtons(buttons, step);
    }
    if (cmd == "wait") {
        step.kind = Step::Wait;
        ss >> step.holdMs;
        return true;
    }
    if (cmd == "scan" || cmd == "rescan") {
        step.kind = cmd == "scan" ? Step::Scan : Step::Rescan;
        ss >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "trace") {
        step.kind = Step::Trace;
        uint32_t rva;
        while (ss >> std::hex >> rva) step.rvas.push_back(rva);
        return !step.rvas.empty();
    }
    if (cmd == "tracereport") {
        step.kind = Step::TraceReport;
        return true;
    }
    if (cmd == "scanrecord") {
        step.kind = Step::ScanRecord;
        ss >> std::hex >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "scanshow") {
        step.kind = Step::ScanShow;
        return true;
    }
    if (cmd == "scansmall") {
        step.kind = Step::ScanSmall;
        return true;
    }
    if (cmd == "scandelta") {
        step.kind = Step::ScanDelta;
        ss >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "setup") {
        // setup <code> <costume> <color> <comment> <vskill> <vtrigger> (x2) <stage>, as the menus show them;
        // <code>:<n> also sets the costume variation
        step.kind = Step::Setup;
        for (auto& p : step.setup.players) {
            std::string code;
            int costume, color, comment, vskill, vtrigger;
            ss >> code >> costume >> color >> comment >> vskill >> vtrigger;
            if (const size_t colon = code.find(':'); colon != std::string::npos) {
                p.variation = static_cast<uint8_t>(std::atoi(code.c_str() + colon + 1));
                code.resize(colon);
            }
            p.character = PackCode(code.c_str());
            p.costume = static_cast<uint16_t>(costume);
            p.color = static_cast<uint8_t>(color - 1);
            p.comment = static_cast<uint8_t>(comment);
            p.vskill = static_cast<uint8_t>(vskill - 1);
            p.vtrigger = static_cast<uint8_t>(vtrigger - 1);
        }
        int stage = 0;
        ss >> stage;
        step.setup.stage = static_cast<uint16_t>(stage);
        return !ss.fail();
    }
    if (cmd == "host" || cmd == "join" || cmd == "spectate" || cmd == "server" || cmd == "packlogs" || cmd == "update" || cmd == "name" || cmd == "delay" || cmd == "transport" || cmd == "ready" || cmd == "choose" || cmd == "leave" || cmd == "netsim" || cmd == "mash" || cmd == "dumps" || cmd == "recordings" || cmd == "pick" ||
        cmd == "stage" || cmd == "netstatus" || cmd == "battle" || cmd == "setstage" || cmd == "park" || cmd == "peek" || cmd == "battlenow" || cmd == "findptr" || cmd == "findbytes" || cmd == "keyconfigs" || cmd == "enuminfo" || cmd == "owner" || cmd == "members" || cmd == "worldctx" || cmd == "poke" || cmd == "stages" || cmd == "nextstage" || cmd == "recdiff" || cmd == "outsidetest" || cmd == "tasklist" || cmd == "phases" || cmd == "regions" || cmd == "costumes" || cmd == "variants" || cmd == "presets" || cmd == "freeplay" || cmd == "relayonly" || cmd == "vsync" || cmd == "uniqueparam" || cmd == "datatable" || cmd == "replay" || cmd == "classes" || cmd == "classinfo" || cmd == "named" || cmd == "scene" || cmd == "relocate" || cmd == "menu" || cmd == "overlay" || cmd == "charas" || cmd == "find" || cmd == "watchregion" || cmd == "packages" || cmd == "preload" || cmd == "charcache" || cmd == "drawlog" || cmd == "inputlog" || cmd == "mappedlog" || cmd == "versions" || cmd == "uipads" || cmd == "uistart") {
        step.kind = Step::Net;
        step.line = line;
        return true;
    }
    if (cmd == "showsetup" || cmd == "clearsetup") {
        step.kind = cmd == "showsetup" ? Step::ShowSetup : Step::ClearSetup;
        return true;
    }
    if (cmd == "connect" || cmd == "disconnect") {
        step.kind = cmd == "connect" ? Step::Connect : Step::Disconnect;
        ss >> step.user;
        return step.user >= 0 && step.user < XUSER_MAX_COUNT;
    }
    step.kind = Step::Game;
    GameCommand& c = step.request.command;
    if (cmd == "save") c = GameCommand::Save;
    else if (cmd == "load") c = GameCommand::Load;
    else if (cmd == "ringsave") c = GameCommand::RingSave;
    else if (cmd == "ringload") c = GameCommand::RingLoad;
    else if (cmd == "layout") c = GameCommand::Layout;
    else if (cmd == "watch") {
        c = GameCommand::Watch;
        step.request.arg2 = 4;
        ss >> step.request.arg0 >> std::hex >> step.request.arg1 >> std::dec >> step.request.arg2;
    }
    else if (cmd == "watchaddr") {
        c = GameCommand::WatchAddr;
        step.request.arg2 = 4;
        std::string rw;
        ss >> std::hex >> step.request.address >> std::dec >> step.request.arg2 >> rw;
        step.request.flag = rw == "rw";
    }
    else if (cmd == "watchreport") c = GameCommand::WatchReport;
    else if (cmd == "stress" || cmd == "learnnoise") {
        c = cmd == "stress" ? GameCommand::Stress : GameCommand::LearnNoise;
        step.request.arg0 = 600;
        step.request.arg1 = 7;
        step.request.arg2 = 1;
        ss >> step.request.arg0 >> step.request.arg1 >> step.request.arg2;
    }
    else if (cmd == "unwatch") c = GameCommand::Unwatch;
    else if (cmd == "synctest") {
        c = GameCommand::SyncTest;
        step.request.arg0 = 300;
        step.request.arg1 = 1;
        ss >> step.request.arg0 >> step.request.arg1;
    }
    else return false;
    return true;
}

void Run(const Step& s)
{
    switch (s.kind) {
    case Step::Press: {
        const int first = s.user == kBothPads ? 0 : s.user;
        const int last = s.user == kBothPads ? 1 : s.user;
        for (int u = first; u <= last; u++) {
            VirtualPad& pad = g_pads[u];
            pad.connected = true;
            pad.buttons = s.buttons;
            pad.lt = s.lt;
            pad.rt = s.rt;
            pad.packet++;
        }
        // A negative hold leaves them pressed (until the next pad step).
        if (s.holdMs < 0) break;
        Sleep(s.holdMs);
        for (int u = first; u <= last; u++) {
            VirtualPad& pad = g_pads[u];
            pad.buttons = 0;
            pad.lt = 0;
            pad.rt = 0;
            pad.packet++;
        }
        Sleep(s.gapMs);
        break;
    }
    case Step::Wait:
        Sleep(s.holdMs);
        break;
    case Step::Connect:
        g_pads[s.user].connected = true;
        break;
    case Step::Setup:
        SetPendingSetup(s.setup);
        Log("setup pending: %s", DescribeSetup(s.setup).c_str());
        break;
    case Step::Net:
        RunNetCommand(s.line);
        break;
    case Step::ShowSetup:
        LogBattleSetup("now");
        break;
    case Step::ClearSetup:
        ClearPendingSetup();
        Log("setup cleared");
        break;
    case Step::Scan:
        ScanStart(s.holdMs);
        break;
    case Step::Rescan:
        ScanNext(s.holdMs);
        break;
    case Step::ScanShow:
        ScanShow();
        break;
    case Step::ScanSmall:
        ScanSmall();
        break;
    case Step::ScanDelta:
        ScanDelta(s.holdMs);
        break;
    case Step::ScanRecord:
        ScanRecordFilter(static_cast<uint32_t>(s.holdMs));
        break;
    case Step::Trace:
        TraceFunctions(g_imageBase, s.rvas);
        break;
    case Step::TraceReport:
        TraceReport(g_imageBase);
        break;
    case Step::Disconnect:
        g_pads[s.user].connected = false;
        break;
    case Step::Game: {
        // Watchpoints by address work on every thread from here, also
        // outside battles (menus have no game-thread hook to run them).
        const GameCommand c = s.request.command;
        if (c == GameCommand::WatchAddr || c == GameCommand::WatchReport || c == GameCommand::Unwatch) {
            if (c == GameCommand::WatchAddr) {
                ArmWatch(0, reinterpret_cast<const void*>(s.request.address), s.request.arg2, g_imageBase,
                         s.request.flag);
            }
            if (c == GameCommand::WatchReport) WatchReport();
            if (c == GameCommand::Unwatch) DisarmWatches();
            break;
        }
        std::lock_guard lock(g_mutex);
        g_gameRequests.push_back(s.request);
        break;
    }
    }
}

void CommandLoop(std::wstring path)
{
    ThreadGuard running;
    uint64_t lastPolls[XUSER_MAX_COUNT] = {};
    ULONGLONG lastReport = GetTickCount64();
    ULONGLONG lastTitle = 0;
    for (;;) {
        std::ifstream in(path);
        if (Stopping()) return;
        NetPollHotkeys();
        // The window procedure (overlay input, pause blocking) also without
        // always-active, once the window exists.
        if (!g_gameWindow && !g_activePending) {
            if (HWND w = FindGameWindow()) {
                g_gameWindow = w;
                SubclassWindow();
            }
        }
        if (g_activePending) {
            if (HWND w = FindGameWindow()) {
                g_gameWindow = w;
                g_activePending = false;
                KeepWindowActive();
                RefreshStatus();
                Log("control: always active on (window %p)", w);
            }
        }
        if (in) {
            std::string line;
            std::deque<Step> steps;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                Step s;
                if (Parse(line, s)) steps.push_back(s);
                else Log("cmd: cannot parse '%s'", line.c_str());
            }
            in.close();
            DeleteFileW(path.c_str());
            Log("cmd: running %zu steps", steps.size());
            for (const Step& s : steps) {
                if (Stopping()) return;
                Run(s);
            }
        }
        // The game sets its own title for a while after starting; keep ours.
        if (!g_activePending && GetTickCount64() - lastTitle > 1000) {
            lastTitle = GetTickCount64();
            RefreshStatus();
        }
        if (GetTickCount64() - lastReport > 60000) {
            lastReport = GetTickCount64();
            char buf[128];
            int n = 0;
            for (int u = 0; u < XUSER_MAX_COUNT; u++) {
                const uint64_t p = g_polls[u];
                n += std::snprintf(buf + n, sizeof(buf) - n, " %llu", static_cast<unsigned long long>(p - lastPolls[u]));
                lastPolls[u] = p;
            }
            Log("xinput polls per 60 s by user:%s; %s", buf, PumpStats().c_str());
        }
        Sleep(50);
    }
}

WNDPROC g_origWndProc = nullptr;
std::atomic<bool> g_alwaysActive{false};

// UE4 stops processing pad input once it is told the app went inactive.
LRESULT CALLBACK ActiveWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    HookGuard guard;
    // The overlay first; what it takes, the game does not see (raw input
    // still needs DefWindowProc to be released).
    if (OverlayWndProc(w, msg, wp, lp)) return msg == WM_INPUT ? DefWindowProcW(w, msg, wp, lp) : 0;
    const bool active = g_alwaysActive;
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (active && !wp) return 0;
        break;
    case WM_ACTIVATE:
        if (active && LOWORD(wp) == WA_INACTIVE) return 0;
        break;
    case WM_KILLFOCUS:
        if (active) return 0;
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
        // Keyboard Start (Enter) and Back (Escape) would pause one side only.
        if (g_blockPause && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
        break;
    case WM_CLOSE:
    case WM_ENDSESSION:
        // The game is closing: tell the others now (Bye) rather than let
        // them wait for the silence timeout.
        NetLeave();
        break;
    default:
        break;
    }
    return CallWindowProcW(g_origWndProc, w, msg, wp, lp);
}

} // namespace

HWND RealForegroundWindow() { return g_origForeground ? g_origForeground() : GetForegroundWindow(); }

void SetPadIsolation(bool isolate) { g_isolatePads = isolate; }

PackedInput LocalPadInput(int user) { return user >= 0 && user < XUSER_MAX_COUNT ? g_padInput[user].load() : 0; }

PackedInput MappedLocalInput(int)
{
    if (!g_origInputUpdate) return LocalPadInput(0);
    // Start as the player pressed it (the pause filter keeps it from the game).
    return g_mappedInput.load() | (g_padInput[0].load() & kStart);
}

void SetBlockPause(bool block) { g_blockPause = block; }

void PressUiButtons(uint16_t buttons, int ms)
{
    g_uiButtons = buttons;
    g_uiButtonsUntil = GetTickCount64() + ms;
}

void RestoreWindowProc()
{
    if (g_origWndProc && g_gameWindow) {
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
    }
}

namespace {

void SubclassWindow()
{
    if (!g_gameWindow || g_origWndProc) return;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ActiveWndProc)));
}

void KeepWindowActive()
{
    g_alwaysActive = true;
    SubclassWindow();
    // It may already have been told it is inactive.
    PostMessageW(g_gameWindow, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_gameWindow, WM_SETFOCUS, 0, 0);
}

} // namespace

bool InstallControl(const std::wstring& commandFile, int virtualPads, bool alwaysActive)
{
    g_gameWindow = FindGameWindow();
    SubclassWindow();
    if (alwaysActive) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto hook = [&](const char* name, void* detour, HwndFn* original) {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
            return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
                   MH_EnableHook(target) == MH_OK;
        };
        const bool ok = hook("GetForegroundWindow", reinterpret_cast<void*>(&ForegroundDetour), &g_origForeground) &&
                        hook("GetActiveWindow", reinterpret_cast<void*>(&ActiveDetour), &g_origActive) &&
                        hook("GetFocus", reinterpret_cast<void*>(&FocusDetour), &g_origFocus);
        if (ok && g_gameWindow) KeepWindowActive();
        else if (ok) g_activePending = true; // the command loop finishes it once the window exists
        Log("control: always active %s (window %p)", !ok ? "FAILED" : g_gameWindow ? "on" : "waiting for the window",
            g_gameWindow.load());
    }
    for (int u = 0; u < virtualPads && u < XUSER_MAX_COUNT; u++) g_pads[u].connected = true;
    HMODULE xinput = LoadLibraryW(L"C:\\Windows\\System32\\xinput1_3.dll");
    void* target = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&GetStateDetour),
                                 reinterpret_cast<void**>(&g_origGetState)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("control: cannot hook XInputGetState");
        return false;
    }
    auto* update = reinterpret_cast<uint8_t*>(g_imageBase + kInputManagerUpdate);
    if (std::memcmp(update, kInputManagerUpdateBytes, sizeof(kInputManagerUpdateBytes)) != 0 ||
        MH_CreateHook(update, reinterpret_cast<void*>(&InputUpdateDetour), reinterpret_cast<void**>(&g_origInputUpdate)) != MH_OK ||
        MH_EnableHook(update) != MH_OK) {
        Log("control: cannot hook the input manager; netplay reads the pads with the default layout");
        g_origInputUpdate = nullptr;
    }
    std::thread(CommandLoop, commandFile).detach();

    // The game only polls pads it has seen connected. After a hot reload it has
    // seen ours disconnect; a device change makes it enumerate them again.
    EnumWindows(
        [](HWND w, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(w)) PostMessageW(w, WM_DEVICECHANGE, 0x0007, 0);
            return TRUE;
        },
        0);
    Log("control: virtual pads ready, commands from sfv_cmd.txt");
    return true;
}

GameRequest TakeGameRequest()
{
    std::lock_guard lock(g_mutex);
    if (g_gameRequests.empty()) return {};
    const GameRequest r = g_gameRequests.front();
    g_gameRequests.pop_front();
    return r;
}

void SetScriptedPad(int user, bool enabled, uint16_t buttons, uint8_t rightTrigger)
{
    if (user < 0 || user >= XUSER_MAX_COUNT) return;
    VirtualPad& pad = g_pads[user];
    pad.scriptedButtons = buttons;
    pad.scriptedRt = rightTrigger;
    pad.scriptedAt = GetTickCount64();
    pad.scripted = enabled;
    pad.packet++;
}

uint64_t PadPolls(int user)
{
    return user >= 0 && user < XUSER_MAX_COUNT ? g_polls[user].load() : 0;
}

} // namespace sfv::mod
