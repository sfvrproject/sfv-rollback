#pragma once
// Shutdown support for hot reload: threads poll Stopping(), and every hook
// detour holds a HookGuard so the DLL is not unloaded while game threads are
// still running its code.

#include <atomic>

namespace sfv::mod {

inline std::atomic<bool> g_stopping{false};
inline std::atomic<int> g_inDetour{0};
inline std::atomic<int> g_threads{0};

inline bool Stopping() { return g_stopping.load(); }

struct HookGuard {
    HookGuard() { g_inDetour++; }
    ~HookGuard() { g_inDetour--; }
    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;
};

// Counts a mod thread for as long as it runs.
struct ThreadGuard {
    ThreadGuard() { g_threads++; }
    ~ThreadGuard() { g_threads--; }
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
};

} // namespace sfv::mod
