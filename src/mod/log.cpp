#include "mod/log.h"

#include <windows.h>

#include <algorithm>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace sfv::mod {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::deque<std::string> g_recent; // the last lines, for the overlay
constexpr size_t kRecentLines = 200;
} // namespace

void LogOpen(const std::wstring& path)
{
    std::lock_guard lock(g_mutex);
    if (!g_file) g_file = _wfopen(path.c_str(), L"a");
}

void LogClose()
{
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void Log(const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "%02d:%02d:%02d.%03d [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                 GetCurrentThreadId(), msg);
    std::fflush(g_file);
    char line[1100];
    std::snprintf(line, sizeof(line), "%02d:%02d:%02d %s", t.wHour, t.wMinute, t.wSecond, msg);
    g_recent.emplace_back(line);
    if (g_recent.size() > kRecentLines) g_recent.pop_front();
}

std::vector<std::string> RecentLog(size_t lines)
{
    std::lock_guard lock(g_mutex);
    const size_t n = std::min(lines, g_recent.size());
    return std::vector<std::string>(g_recent.end() - static_cast<std::ptrdiff_t>(n), g_recent.end());
}

} // namespace sfv::mod
