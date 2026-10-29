#pragma once
// Logs the current call stack as RVAs of the game image, for finding callers.

#include "mod/log.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace sfv::mod {

inline void LogStack(const char* label, uintptr_t imageBase)
{
    void* frames[40];
    const USHORT n = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    std::string line;
    char buf[32];
    for (USHORT i = 0; i < n; i++) {
        const auto a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a >= imageBase && a < imageBase + 0x5000000) std::snprintf(buf, sizeof(buf), " %llX", static_cast<unsigned long long>(a - imageBase));
        else std::snprintf(buf, sizeof(buf), " ext");
        line += buf;
    }
    Log("stack %s (thread %lu):%s", label, GetCurrentThreadId(), line.c_str());
}

} // namespace sfv::mod
