#include "mod/boot.h"

#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/sfv_offsets.h"
#include "mod/ue4.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace sfv::mod {

namespace {

// Blueprint GetLoginState's native body: online subsystem -> identity ->
// GetLoginStatus(local user).
constexpr uintptr_t kGetLoginStatus = 0x73BB90;
// InitialState's step "continue offline": clears the online mode flag (global,
// rva 0x3A91FC1, via 0x69D800), marks the scene manager (+0x451 = 0) and
// switches to the main menu. The state schedules it itself (step function at
// +0x338) when it gives up on the login.
constexpr uintptr_t kInitialStateGoOffline = 0x5E4ED0;
// Sets OnlineSubsystemKiwi +0x1022, the login dialog's "offline" sets 0.
constexpr uintptr_t kSetOnlineSubsystemFlag = 0x750950;
// Scene states run one step function at a time: +0x338, after +0x378 frames.
constexpr uint32_t kStateStep = 0x338;
constexpr uint32_t kStateStepDelay = 0x378;
// InitialState's steps: local preparation (0x600D20 ... 0x6014A0), then
// 0x601820 starts the login and waits for it (about 30 s when the servers
// cannot be reached). The offline route replaces that step.
constexpr uintptr_t kInitialStateLoginStep = 0x601820;
constexpr int kOfflineWaitFrames = 60 * 60;

uintptr_t g_base = 0;
std::atomic<int> g_lastStatus{-2};

std::string Narrow(const wchar_t* w)
{
    std::string s;
    for (int i = 0; w && w[i] && i < 200; i++) s += w[i] < 128 ? static_cast<char>(w[i]) : '?';
    return s;
}

std::string Address(const sockaddr* a)
{
    char buf[64] = "?";
    if (a && a->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(a);
        const auto* b = reinterpret_cast<const uint8_t*>(&in->sin_addr);
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], ntohs(in->sin_port));
    }
    else if (a && a->sa_family == AF_INET6) {
        std::snprintf(buf, sizeof(buf), "[ipv6]:%u", ntohs(reinterpret_cast<const sockaddr_in6*>(a)->sin6_port));
    }
    return buf;
}

// Network calls, logged with how long they took.
using GetAddrInfoWFn = INT(WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
using GetAddrInfoFn = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using InternetConnectWFn = void*(WINAPI*)(void*, LPCWSTR, WORD, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
using HttpSendRequestWFn = BOOL(WINAPI*)(void*, LPCWSTR, DWORD, LPVOID, DWORD);
using WinHttpConnectFn = void*(WINAPI*)(void*, LPCWSTR, WORD, DWORD);
using WinHttpSendRequestFn = BOOL(WINAPI*)(void*, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);

GetAddrInfoWFn g_origGetAddrInfoW = nullptr;
GetAddrInfoFn g_origGetAddrInfo = nullptr;
ConnectFn g_origConnect = nullptr;
InternetConnectWFn g_origInternetConnectW = nullptr;
HttpSendRequestWFn g_origHttpSendRequestW = nullptr;
WinHttpConnectFn g_origWinHttpConnect = nullptr;
WinHttpSendRequestFn g_origWinHttpSendRequest = nullptr;

INT WSAAPI GetAddrInfoWDetour(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result)
{
    HookGuard guard;
    const auto t0 = GetTickCount64();
    const INT r = g_origGetAddrInfoW(node, service, hints, result);
    Log("net: GetAddrInfoW %s:%s -> %d (%llu ms)", Narrow(node).c_str(), Narrow(service).c_str(), r,
        static_cast<unsigned long long>(GetTickCount64() - t0));
    return r;
}

INT WSAAPI GetAddrInfoDetour(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result)
{
    HookGuard guard;
    const auto t0 = GetTickCount64();
    const INT r = g_origGetAddrInfo(node, service, hints, result);
    Log("net: getaddrinfo %s:%s -> %d (%llu ms)", node ? node : "", service ? service : "", r,
        static_cast<unsigned long long>(GetTickCount64() - t0));
    return r;
}

int WSAAPI ConnectDetour(SOCKET s, const sockaddr* name, int len)
{
    HookGuard guard;
    const auto t0 = GetTickCount64();
    const int r = g_origConnect(s, name, len);
    const int err = r ? WSAGetLastError() : 0;
    Log("net: connect %s -> %d (error %d, %llu ms)", Address(name).c_str(), r, err,
        static_cast<unsigned long long>(GetTickCount64() - t0));
    if (r) WSASetLastError(err);
    return r;
}

void* WINAPI InternetConnectWDetour(void* session, LPCWSTR server, WORD port, LPCWSTR user, LPCWSTR password, DWORD service,
                                    DWORD flags, DWORD_PTR context)
{
    HookGuard guard;
    void* r = g_origInternetConnectW(session, server, port, user, password, service, flags, context);
    Log("net: InternetConnectW %s:%u -> %p", Narrow(server).c_str(), port, r);
    return r;
}

BOOL WINAPI HttpSendRequestWDetour(void* request, LPCWSTR headers, DWORD headersLength, LPVOID optional, DWORD optionalLength)
{
    HookGuard guard;
    const auto t0 = GetTickCount64();
    const BOOL r = g_origHttpSendRequestW(request, headers, headersLength, optional, optionalLength);
    const DWORD err = r ? 0 : GetLastError();
    Log("net: HttpSendRequestW %p -> %d (error %lu, %llu ms)", request, r, err,
        static_cast<unsigned long long>(GetTickCount64() - t0));
    if (!r) SetLastError(err);
    return r;
}

void* WINAPI WinHttpConnectDetour(void* session, LPCWSTR server, WORD port, DWORD reserved)
{
    HookGuard guard;
    void* r = g_origWinHttpConnect(session, server, port, reserved);
    Log("net: WinHttpConnect %s:%u -> %p", Narrow(server).c_str(), port, r);
    return r;
}

BOOL WINAPI WinHttpSendRequestDetour(void* request, LPCWSTR headers, DWORD headersLength, LPVOID optional,
                                     DWORD optionalLength, DWORD totalLength, DWORD_PTR context)
{
    HookGuard guard;
    const auto t0 = GetTickCount64();
    const BOOL r = g_origWinHttpSendRequest(request, headers, headersLength, optional, optionalLength, totalLength, context);
    const DWORD err = r ? 0 : GetLastError();
    Log("net: WinHttpSendRequest %p -> %d (error %lu, %llu ms)", request, r, err,
        static_cast<unsigned long long>(GetTickCount64() - t0));
    if (!r) SetLastError(err);
    return r;
}

bool HookExport(const wchar_t* module, const char* name, void* detour, void** original)
{
    HMODULE m = LoadLibraryW(module);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    const bool ok = target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    if (!ok) Log("boot: cannot hook %s", name);
    return ok;
}

void WatchStep()
{
    const int status = LoginStatus();
    const int last = g_lastStatus.exchange(status);
    if (status != last) Log("boot: login status %d (was %d)", status, last);
    if (!Stopping()) RunOnGameThreadAfter(30, WatchStep);
}

// Waits (once per frame) for InitialState to reach its login step, then
// replaces that step with the offline route.
void OfflineAtLoginStep(uint8_t* state, int frame)
{
    if (Stopping()) return;
    if (!state) {
        if (!ue4::EnsureObjects(g_base)) return;
        ue4::ForEachObject([&](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) == "InitialState") state = static_cast<uint8_t*>(const_cast<void*>(object));
        });
        if (!state) {
            Log("boot: offline: no InitialState");
            return;
        }
    }
    auto* step = reinterpret_cast<uintptr_t*>(state + kStateStep);
    if (*step != g_base + kInitialStateLoginStep) {
        if (frame < kOfflineWaitFrames) RunOnGameThreadAfter(1, [=] { OfflineAtLoginStep(state, frame + 1); });
        else Log("boot: offline: InitialState never reached its login step");
        return;
    }
    Log("boot: offline: InitialState reached its login step at frame %d; continuing offline", frame);
    reinterpret_cast<void (*)(uint8_t)>(g_base + kSetOnlineSubsystemFlag)(0);
    *step = g_base + kInitialStateGoOffline;
    *reinterpret_cast<int32_t*>(state + kStateStepDelay) = 0;
}

} // namespace

void GoOfflineFromTitle()
{
    RunOnGameThreadAfter(1, [] { OfflineAtLoginStep(nullptr, 0); });
}

using InFreePlayPeriodFn = bool (*)(void* info);
InFreePlayPeriodFn g_origInFreePlayPeriod = nullptr;
std::atomic<int> g_freePlayAnswer{-1}; // the game's own last answer, for the log
// Research: `freeplay off` lets the game decide, `freeplay fail` says ended.
enum class FreePlay { Guard, Game, Fail };
std::atomic<FreePlay> g_freePlay{FreePlay::Guard};

bool InFreePlayPeriodDetour(void* info)
{
    HookGuard guard;
    const int answer = g_origInFreePlayPeriod(info) ? 1 : 0;
    if (g_freePlay != FreePlay::Guard) {
        Log("boot: the battle's free play check: the game says %s (%s)", answer ? "in the period" : "ended",
            g_freePlay == FreePlay::Fail ? "answered ended" : "guard off");
        return g_freePlay == FreePlay::Game && answer != 0;
    }
    if (g_freePlayAnswer.exchange(answer) != answer) {
        Log("boot: the battle's free play check: the game says %s; playable while the mod is loaded",
            answer ? "in the period" : "ended");
    }
    return true;
}

void SetFreePlayGuard(const std::string& mode)
{
    g_freePlay = mode == "off" ? FreePlay::Game : mode == "fail" ? FreePlay::Fail : FreePlay::Guard;
    Log("boot: free play guard %s", mode.c_str());
}

bool InstallFreePlayGuard(const GameImage& image)
{
    const game::CodeSite& site = game::kInFreePlayPeriod;
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("boot: signature mismatch at %s; the free play check stays", site.name);
        return false;
    }
    const bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&InFreePlayPeriodDetour),
                                  reinterpret_cast<void**>(&g_origInFreePlayPeriod)) == MH_OK &&
                    MH_EnableHook(target) == MH_OK;
    if (!ok) Log("boot: cannot hook %s", site.name);
    return ok;
}

bool InstallBootProbes(const GameImage& image, bool logNetwork)
{
    g_base = image.base;
    if (!logNetwork) return true;
    bool ok = HookExport(L"ws2_32.dll", "GetAddrInfoW", reinterpret_cast<void*>(&GetAddrInfoWDetour),
                         reinterpret_cast<void**>(&g_origGetAddrInfoW));
    ok &= HookExport(L"ws2_32.dll", "getaddrinfo", reinterpret_cast<void*>(&GetAddrInfoDetour),
                     reinterpret_cast<void**>(&g_origGetAddrInfo));
    ok &= HookExport(L"ws2_32.dll", "connect", reinterpret_cast<void*>(&ConnectDetour), reinterpret_cast<void**>(&g_origConnect));
    ok &= HookExport(L"wininet.dll", "InternetConnectW", reinterpret_cast<void*>(&InternetConnectWDetour),
                     reinterpret_cast<void**>(&g_origInternetConnectW));
    ok &= HookExport(L"wininet.dll", "HttpSendRequestW", reinterpret_cast<void*>(&HttpSendRequestWDetour),
                     reinterpret_cast<void**>(&g_origHttpSendRequestW));
    ok &= HookExport(L"winhttp.dll", "WinHttpConnect", reinterpret_cast<void*>(&WinHttpConnectDetour),
                     reinterpret_cast<void**>(&g_origWinHttpConnect));
    ok &= HookExport(L"winhttp.dll", "WinHttpSendRequest", reinterpret_cast<void*>(&WinHttpSendRequestDetour),
                     reinterpret_cast<void**>(&g_origWinHttpSendRequest));
    Log("network logging %s", ok ? "on" : "partly on");
    return ok;
}

int LoginStatus()
{
    using Fn = uint8_t (*)();
    return reinterpret_cast<Fn>(g_base + kGetLoginStatus)();
}

void WatchLoginStatus()
{
    RunOnGameThread(WatchStep);
}

} // namespace sfv::mod
