#pragma once

#include <string>
#include <vector>

namespace sfv::mod {

// The event log, SFVR\logs\sfvr.log (files.h). Thread-safe. Lines carry the
// time of day; the file starts with the date ("SFVR log started ...", which
// RotateLog reads) and says when the date changes.
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogClose();
// The last `lines` lines logged, oldest first (for the overlay).
std::vector<std::string> RecentLog(size_t lines);
// A match's own log: from LogCaptureStart, every line logged is also kept
// (the newest kCapturedLines), until LogCaptureTake hands them over and
// starts again, or LogCaptureStop.
void LogCaptureStart();
std::vector<std::string> LogCaptureTake();
void LogCaptureStop();

} // namespace sfv::mod
