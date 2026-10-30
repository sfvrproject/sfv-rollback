#pragma once
// The SFVR folder next to the game (Binaries\Win64\SFVR): what a player zips
// up and sends when something goes wrong.
//   logs\     the event log: sfvr.log (this run; sfvr.<n>.log for test
//             instance n), earlier runs as sfvr_<start>.log
//   matches\  per match a recording (.sfvr) and its log (.log), named by date,
//             players, characters and match id
// Plus "Pack logs to share": a zip of the logs and the latest matches.

#include <string>
#include <utility>
#include <vector>

namespace sfv::mod {

constexpr size_t kKeptRunLogs = 20;      // earlier runs' event logs
constexpr size_t kKeptRecordings = 300;  // .sfvr files in the matches folder (a match, its desync reports)
constexpr size_t kKeptMatchLogs = 200;   // .log files in the matches folder
constexpr size_t kSharedMatches = 10;    // the latest matches in a shared zip

void SetDataDir(const std::wstring& dir);
std::wstring DataDir();                   // ...\Win64\SFVR
std::wstring LogsDir();                   // ...\SFVR\logs
std::wstring MatchesDir();                // ...\SFVR\matches

// Creates the folder and any missing parents.
bool EnsureDirectory(const std::wstring& path);
// Keeps the `keep` most recently written files matching `pattern` (a
// FindFirstFile pattern such as L"*.sfvr") in `dir` and deletes the others.
void PruneOldest(const std::wstring& dir, const std::wstring& pattern, size_t keep);
// Before a run opens <dir>\<stem>.log: the previous run's file becomes
// <stem>_<when it started>.log (from its first line, else when it was last
// written), and only the newest kKeptRunLogs of those stay.
void RotateLog(const std::wstring& dir, const std::wstring& stem);

// A name part made safe for file names: letters, digits, '-' and '_' kept,
// runs of anything else become one '-', at most `maxLen` characters; `empty`
// when nothing is left.
std::string FileSafe(const std::string& text, size_t maxLen, const std::string& empty);
// A path as UTF-8 (what core/file_open.h opens), every character kept: a game
// or user folder outside ASCII stays a real path.
std::string Utf8Path(const std::wstring& path);

// Writes a zip (stored, not compressed: the recordings already are) of the
// files, each {path on disk, name in the zip}.
bool WriteZip(const std::wstring& path, const std::vector<std::pair<std::wstring, std::string>>& files,
              std::string* error);
// The zip "Pack logs to share" makes: the event logs (this run and the
// earlier ones kept) and the latest kSharedMatches matches' logs and
// recordings, as SFVR\SFVR-logs-<date>.zip. Returns its path, empty on failure.
std::wstring PackLogsToShare(std::string* error);
// Opens the SFVR folder in Explorer, with `select` selected when given.
void OpenDataFolder(const std::wstring& select = L"");

} // namespace sfv::mod
