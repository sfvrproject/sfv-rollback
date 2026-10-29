#pragma once

#include <string>
#include <vector>

namespace sfv::mod {

// Appends to sfv_rollback.log next to the DLL. Thread-safe.
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogClose();
// The last `lines` lines logged, oldest first (for the overlay).
std::vector<std::string> RecentLog(size_t lines);

} // namespace sfv::mod
