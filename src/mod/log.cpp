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
WORD g_day = 0;                    // the day of the last line written (a date line when it changes)
bool g_capturing = false;          // LogCaptureStart
std::deque<std::string> g_captured;
constexpr size_t kCapturedLines = 50000;
} // namespace

void LogOpen(const std::wstring& path)
{
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    g_file = _wfopen(path.c_str(), L"a");
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "SFVR log started %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond);
    std::fflush(g_file);
    g_day = t.wDay;
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
    if (t.wDay != g_day) {
        std::fprintf(g_file, "---- %04d-%02d-%02d\n", t.wYear, t.wMonth, t.wDay);
        g_day = t.wDay;
    }
    char full[1100];
    std::snprintf(full, sizeof(full), "%02d:%02d:%02d.%03d [%5lu] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                  GetCurrentThreadId(), msg);
    std::fprintf(g_file, "%s\n", full);
    std::fflush(g_file);
    if (g_capturing) {
        g_captured.emplace_back(full);
        if (g_captured.size() > kCapturedLines) g_captured.pop_front();
    }
    char line[1100];
    std::snprintf(line, sizeof(line), "%02d:%02d:%02d %s", t.wHour, t.wMinute, t.wSecond, msg);
    g_recent.emplace_back(line);
    if (g_recent.size() > kRecentLines) g_recent.pop_front();
}

void LogCaptureStart()
{
    std::lock_guard lock(g_mutex);
    g_captured.clear();
    g_capturing = true;
}

std::vector<std::string> LogCaptureTake()
{
    std::lock_guard lock(g_mutex);
    std::vector<std::string> out(g_captured.begin(), g_captured.end());
    g_captured.clear();
    return out;
}

void LogCaptureStop()
{
    std::lock_guard lock(g_mutex);
    g_captured.clear();
    g_capturing = false;
}

std::vector<std::string> RecentLog(size_t lines)
{
    std::lock_guard lock(g_mutex);
    const size_t n = std::min(lines, g_recent.size());
    return std::vector<std::string>(g_recent.end() - static_cast<std::ptrdiff_t>(n), g_recent.end());
}

} // namespace sfv::mod
