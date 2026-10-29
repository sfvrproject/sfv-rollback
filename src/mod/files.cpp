#include "mod/files.h"

#include "core/state.h"
#include "mod/log.h"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace sfv::mod {

namespace {

std::wstring g_dataDir;

struct Found {
    std::wstring name;
    FILETIME written{};
};

// The files matching `pattern` in `dir`, newest first.
std::vector<Found> ListNewestFirst(const std::wstring& dir, const std::wstring& pattern)
{
    std::vector<Found> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return CompareFileTime(&a.written, &b.written) > 0; });
    return out;
}

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '_';
    return s;
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>* out)
{
    // Shared: the event log of this run is still open for writing.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    return ok;
}

// MS-DOS date and time, as zip entries carry them.
void DosTime(const FILETIME& ft, uint16_t* time, uint16_t* date)
{
    FILETIME local;
    SYSTEMTIME t;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &t);
    *time = static_cast<uint16_t>(t.wHour << 11 | t.wMinute << 5 | t.wSecond / 2);
    *date = static_cast<uint16_t>((std::max<int>(t.wYear, 1980) - 1980) << 9 | t.wMonth << 5 | t.wDay);
}

void Put16(std::vector<uint8_t>& b, uint16_t v) { b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8)}); }
void Put32(std::vector<uint8_t>& b, uint32_t v)
{
    b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
}

std::string Stamp(const SYSTEMTIME& t)
{
    char s[32];
    std::snprintf(s, sizeof(s), "%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

} // namespace

void SetDataDir(const std::wstring& dir) { g_dataDir = dir; }
std::wstring DataDir() { return g_dataDir; }
std::wstring LogsDir() { return g_dataDir + L"\\logs"; }
std::wstring MatchesDir() { return g_dataDir + L"\\matches"; }

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) EnsureDirectory(path.substr(0, slash));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void PruneOldest(const std::wstring& dir, const std::wstring& pattern, size_t keep)
{
    const auto files = ListNewestFirst(dir, pattern);
    for (size_t i = keep; i < files.size(); i++) DeleteFileW((dir + L"\\" + files[i].name).c_str());
}

void RotateLog(const std::wstring& dir, const std::wstring& stem)
{
    const std::wstring path = dir + L"\\" + stem + L".log";
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    // When the run started: its first line (LogOpen), else when it last wrote.
    SYSTEMTIME t{};
    bool known = false;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char line[96] = {};
        if (std::fgets(line, sizeof(line), f)) {
            int y, mo, d, h, mi, s;
            if (std::sscanf(line, "SFVR log started %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
                t = {WORD(y), WORD(mo), 0, WORD(d), WORD(h), WORD(mi), WORD(s), 0};
                known = true;
            }
        }
        std::fclose(f);
    }
    if (!known) {
        FILETIME local;
        FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
    }
    std::wstring target = dir + L"\\" + stem + L"_" + Wide(Stamp(t)) + L".log";
    for (int n = 2; GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; n++) {
        target = dir + L"\\" + stem + L"_" + Wide(Stamp(t)) + L"-" + std::to_wstring(n) + L".log";
    }
    MoveFileExW(path.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED);
    PruneOldest(dir, stem + L"_*.log", kKeptRunLogs);
}

std::string FileSafe(const std::string& text, size_t maxLen, const std::string& empty)
{
    std::string out;
    for (unsigned char c : text) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (keep) out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '-') out += '-';
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? empty : out;
}

bool WriteZip(const std::wstring& path, const std::vector<std::pair<std::wstring, std::string>>& files,
              std::string* error)
{
    std::vector<uint8_t> zip, central;
    uint16_t entries = 0;
    for (const auto& [source, name] : files) {
        std::vector<uint8_t> data;
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!ReadWhole(source, &data) || !GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &info)) continue;
        uint16_t time, date;
        DosTime(info.ftLastWriteTime, &time, &date);
        const uint32_t crc = Crc32(data.data(), data.size());
        const uint32_t offset = static_cast<uint32_t>(zip.size());
        const uint32_t size = static_cast<uint32_t>(data.size());
        // Local header, then the bytes as they are (method 0: stored).
        Put32(zip, 0x04034b50);
        Put16(zip, 20);
        Put16(zip, 0);
        Put16(zip, 0);
        Put16(zip, time);
        Put16(zip, date);
        Put32(zip, crc);
        Put32(zip, size);
        Put32(zip, size);
        Put16(zip, static_cast<uint16_t>(name.size()));
        Put16(zip, 0);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), data.begin(), data.end());
        // Its central directory entry.
        Put32(central, 0x02014b50);
        Put16(central, 20);
        Put16(central, 20);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, time);
        Put16(central, date);
        Put32(central, crc);
        Put32(central, size);
        Put32(central, size);
        Put16(central, static_cast<uint16_t>(name.size()));
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put32(central, 0);
        Put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
        entries++;
    }
    const uint32_t centralAt = static_cast<uint32_t>(zip.size());
    zip.insert(zip.end(), central.begin(), central.end());
    Put32(zip, 0x06054b50);
    Put16(zip, 0);
    Put16(zip, 0);
    Put16(zip, entries);
    Put16(zip, entries);
    Put32(zip, static_cast<uint32_t>(central.size()));
    Put32(zip, centralAt);
    Put16(zip, 0);

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        if (error) *error = "cannot create " + Narrow(path);
        return false;
    }
    const bool written = std::fwrite(zip.data(), 1, zip.size(), f) == zip.size();
    std::fclose(f);
    if (!written && error) *error = "cannot write " + Narrow(path);
    return written;
}

std::wstring PackLogsToShare(std::string* error)
{
    std::vector<std::pair<std::wstring, std::string>> files;
    // The event logs: this run's and the earlier ones kept.
    for (const auto& f : ListNewestFirst(LogsDir(), L"*.log")) files.push_back({LogsDir() + L"\\" + f.name, "logs/" + Narrow(f.name)});
    // The latest matches: their logs, and every recording with the same match
    // id (the match's own and its desync reports). File names are
    // <date>_<time>_<P1>_vs_<P2>_<id>_<side>[_<kind>], with '_' only between
    // the parts.
    std::set<std::string> ids;
    const auto logs = ListNewestFirst(MatchesDir(), L"*.log");
    for (size_t i = 0; i < logs.size() && i < kSharedMatches; i++) {
        const std::string name = Narrow(logs[i].name);
        files.push_back({MatchesDir() + L"\\" + logs[i].name, "matches/" + name});
        size_t at = 0;
        for (int part = 0; part < 5 && at != std::string::npos; part++) at = name.find('_', at + 1);
        if (at != std::string::npos) ids.insert(name.substr(at + 1, name.find('_', at + 1) - at - 1));
    }
    for (const auto& f : ListNewestFirst(MatchesDir(), L"*.sfvr")) {
        const std::string name = Narrow(f.name);
        for (const auto& id : ids) {
            if (name.find("_" + id + "_") == std::string::npos) continue;
            files.push_back({MatchesDir() + L"\\" + f.name, "matches/" + name});
            break;
        }
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring path = DataDir() + L"\\SFVR-logs-" + Wide(Stamp(t)) + L".zip";
    if (!WriteZip(path, files, error)) return L"";
    PruneOldest(DataDir(), L"SFVR-logs-*.zip", 5);
    Log("files: packed %zu files to share into %s", files.size(), Narrow(path).c_str());
    return path;
}

void OpenDataFolder(const std::wstring& select)
{
    const std::wstring args = select.empty() ? L"\"" + DataDir() + L"\"" : L"/select,\"" + select + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace sfv::mod
