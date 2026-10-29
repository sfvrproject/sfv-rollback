#pragma once
// Shared by the loader proxies: a thread loads sfv_rollback.dll from the
// proxy's own folder and reloads it on request.
//
// The mod is loaded from a copy (sfv_rollback.live.<instance>.<n>.dll) so the
// build can overwrite sfv_rollback.dll while the game runs. Creating the file
// sfv_reload (instance 0) or sfv_reload.<instance> makes the loader call the
// mod's SfvShutdown export, unload it, and load a fresh copy, unless the
// mod's SfvCanUnload export says no (a netplay session is on): then the
// request is dropped.
//
// Several game instances can share the folder: each takes the lowest free
// instance number (named mutex) and tells the mod through SFV_INSTANCE. With
// [mod] multi_instance=1 in sfv_rollback.ini the game's own single-instance
// check (a mutex named "StreetFighterV") is made per process. Two instances
// write the same save files, so this is for testing only.

#include <windows.h>

namespace sfv::proxy {

inline wchar_t g_dir[MAX_PATH];
inline int g_instance = 0;
inline decltype(&CreateMutexW) g_realCreateMutexW = nullptr;

inline HANDLE WINAPI CreateMutexWHook(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
    if (name && lstrcmpW(name, L"StreetFighterV") == 0) {
        wchar_t unique[64];
        wsprintfW(unique, L"StreetFighterV-%lu", GetCurrentProcessId());
        return g_realCreateMutexW(attributes, owner, unique);
    }
    return g_realCreateMutexW(attributes, owner, name);
}

// Points the exe's CreateMutexW import at the hook above.
inline void AllowMultipleInstances()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; desc++) {
        if (lstrcmpiA(reinterpret_cast<char*>(base + desc->Name), "KERNEL32.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<char*>(byName->Name), "CreateMutexW") != 0) continue;
            DWORD old;
            if (VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old)) {
                g_realCreateMutexW = reinterpret_cast<decltype(&CreateMutexW)>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<ULONGLONG>(&CreateMutexWHook);
                VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), old, &old);
            }
            return;
        }
    }
}

inline DWORD WINAPI LoadMod(LPVOID)
{
    wchar_t source[MAX_PATH], trigger[MAX_PATH], live[MAX_PATH];
    wsprintfW(source, L"%ssfv_rollback.dll", g_dir);
    if (g_instance == 0) wsprintfW(trigger, L"%ssfv_reload", g_dir);
    else wsprintfW(trigger, L"%ssfv_reload.%d", g_dir, g_instance);

    HMODULE mod = nullptr;
    for (int generation = 0;; generation++) {
        wsprintfW(live, L"%ssfv_rollback.live.%d.%d.dll", g_dir, g_instance, generation);
        DeleteFileW(live);
        mod = CopyFileW(source, live, FALSE) ? LoadLibraryW(live) : nullptr;

        for (;;) {
            while (GetFileAttributesW(trigger) == INVALID_FILE_ATTRIBUTES) Sleep(250);
            DeleteFileW(trigger);
            using CanUnloadFn = bool (*)();
            auto canUnload = mod ? reinterpret_cast<CanUnloadFn>(GetProcAddress(mod, "SfvCanUnload")) : nullptr;
            if (!canUnload || canUnload()) break;
        }

        if (mod) {
            using ShutdownFn = void (*)();
            if (auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(mod, "SfvShutdown"))) shutdown();
            FreeLibrary(mod);
            DeleteFileW(live);
        }
    }
}

inline void Start(HMODULE self)
{
    DWORD n = GetModuleFileNameW(self, g_dir, MAX_PATH);
    while (n > 0 && g_dir[n - 1] != L'\\' && g_dir[n - 1] != L'/') n--;
    g_dir[n] = 0;
    if (n + 40 >= MAX_PATH) return;

    for (g_instance = 0; g_instance < 8; g_instance++) {
        wchar_t name[64];
        wsprintfW(name, L"Local\\sfv_rollback_instance_%d", g_instance);
        HANDLE m = CreateMutexW(nullptr, TRUE, name);
        if (m && GetLastError() != ERROR_ALREADY_EXISTS) break; // kept open for the process lifetime
        if (m) CloseHandle(m);
    }
    wchar_t value[8];
    wsprintfW(value, L"%d", g_instance);
    SetEnvironmentVariableW(L"SFV_INSTANCE", value);

    wchar_t ini[MAX_PATH];
    wsprintfW(ini, L"%ssfv_rollback.ini", g_dir);
    if (GetPrivateProfileIntW(L"mod", L"multi_instance", 0, ini)) AllowMultipleInstances();

    if (HANDLE t = CreateThread(nullptr, 0, LoadMod, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace sfv::proxy

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        sfv::proxy::Start(module);
    }
    return TRUE;
}
