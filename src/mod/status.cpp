#include "mod/status.h"

#include <windows.h>

#include <mutex>

namespace sfv::mod {

namespace {

std::mutex g_mutex;
int g_instance = 0;
std::string g_text;

BOOL CALLBACK Apply(HWND w, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (lstrcmpW(cls, L"UnrealWindow") != 0) return TRUE;

    std::string title = "StreetFighterV [sfv " + std::to_string(g_instance) + "]";
    if (!g_text.empty()) title += " " + g_text;
    std::wstring wide(title.begin(), title.end());
    // InternalGetWindowText: GetWindowText would send WM_GETTEXT to the game
    // thread, which may be waiting for g_mutex (SetStatus): a deadlock.
    wchar_t current[256] = {};
    InternalGetWindowText(w, current, 256);
    if (wide == current) return TRUE;
    // SendMessageTimeout: the window thread may be busy; never block on it.
    SendMessageTimeoutW(w, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wide.c_str()), SMTO_ABORTIFHUNG, 200, nullptr);
    return TRUE;
}

} // namespace

void SetStatusInstance(int instance)
{
    std::lock_guard lock(g_mutex);
    g_instance = instance;
    EnumWindows(Apply, 0);
}

std::string StatusText()
{
    std::lock_guard lock(g_mutex);
    return g_text;
}

int StatusInstance()
{
    std::lock_guard lock(g_mutex);
    return g_instance;
}

void RefreshStatus()
{
    std::lock_guard lock(g_mutex);
    EnumWindows(Apply, 0);
}

void SetStatus(const std::string& text)
{
    std::lock_guard lock(g_mutex);
    if (text == g_text) return;
    g_text = text;
    EnumWindows(Apply, 0);
}

} // namespace sfv::mod
