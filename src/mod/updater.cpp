#include "mod/updater.h"

#include "core/update_info.h"
#include "mod/files.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/update_key.h"

#include <windows.h>

#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sfv::mod {

namespace {

constexpr size_t kMaxInfoBytes = 64 * 1024;
constexpr DWORD kTimeoutMs = 8000;

std::mutex g_mutex;
UpdateView g_view;         // under g_mutex
UpdateInfo g_info;         // under g_mutex: the info `g_view.latest` came from
std::string g_configured;  // the configured site
std::wstring g_gameDir;
std::atomic<bool> g_busy{false};

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '_';
    return s;
}

void Set(UpdateView::State state, const std::string& message = "")
{
    std::lock_guard lock(g_mutex);
    g_view.state = state;
    g_view.message = message;
}

// GET `url` (http or https) into `out`, at most `maxBytes`; `progress` gets
// the bytes so far.
bool HttpGet(const std::string& url, size_t maxBytes, std::vector<uint8_t>* out, std::string* error,
             const std::function<void(uint64_t)>& progress = nullptr)
{
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        *error = "bad address " + url;
        return false;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET session = WinHttpOpen(L"SFVR updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    bool ok = false;
    if (request) {
        WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != 200) *error = "the site answered " + std::to_string(status) + " for " + url;
            else {
                out->clear();
                ok = true;
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available)) {
                        ok = false;
                        *error = "the download of " + url + " broke off";
                        break;
                    }
                    if (available == 0) break;
                    if (out->size() + available > maxBytes) {
                        ok = false;
                        *error = url + " is larger than expected";
                        break;
                    }
                    const size_t at = out->size();
                    out->resize(at + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, out->data() + at, available, &read)) {
                        ok = false;
                        *error = "the download of " + url + " broke off";
                        break;
                    }
                    out->resize(at + read);
                    if (progress) progress(out->size());
                    if (Stopping()) {
                        ok = false;
                        *error = "stopped";
                        break;
                    }
                }
            }
        }
        else {
            *error = "cannot reach " + url + " (error " + std::to_string(GetLastError()) + ")";
        }
    }
    else {
        *error = "cannot open " + url;
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

std::string SiteUrl()
{
    std::lock_guard lock(g_mutex);
    return g_view.url;
}

void Check()
{
    ThreadGuard running;
    // Installed in this run: nothing more to do until the next launch.
    if (UpdateViewNow().state == UpdateView::State::Ready) return;
    const std::string url = SiteUrl();
    Set(UpdateView::State::Checking);
    std::vector<uint8_t> body, sig;
    std::string error;
    if (!HttpGet(url, kMaxInfoBytes, &body, &error) || !HttpGet(url + ".sig", 4096, &sig, &error)) {
        Log("update: %s", error.c_str());
        Set(UpdateView::State::Failed, error);
        return;
    }
    // The signature first: nothing in the file counts before it checks out.
    const std::vector<uint8_t> signature = Base64Decode(std::string_view(reinterpret_cast<const char*>(sig.data()), sig.size()));
    if (!VerifySignature(body.data(), body.size(), signature.data(), signature.size(), kUpdatePublicKey)) {
        Log("update: the update info at %s is not signed with this build's key; ignored", url.c_str());
        Set(UpdateView::State::Failed, "the update info's signature does not match");
        return;
    }
    UpdateInfo info;
    if (!ParseUpdateInfo(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), &info, &error)) {
        Log("update: cannot read the update info: %s", error.c_str());
        Set(UpdateView::State::Failed, "cannot read the update info: " + error);
        return;
    }
    std::lock_guard lock(g_mutex);
    g_view.latest = info.version;
    g_view.published = info.published;
    g_view.notes = info.notes;
    g_view.message.clear();
    const bool newer = CompareVersions(info.version, g_view.current) > 0;
    g_view.state = newer ? UpdateView::State::Available : UpdateView::State::UpToDate;
    g_info = std::move(info);
    Log("update: the site has SFVR %s (this build %s): %s", g_view.latest.c_str(), g_view.current.c_str(),
        newer ? "an update" : "up to date");
}

// Puts the staged files in place, the old ones aside; all or nothing.
bool Apply(const std::wstring& staging, const UpdateInfo& info, std::string* error)
{
    struct Swap {
        std::wstring target, old;
        bool hadOld;
    };
    std::vector<Swap> done;
    const std::wstring stamp = std::to_wstring(GetTickCount64());
    for (const UpdateFile& f : info.files) {
        const std::wstring target = g_gameDir + L"\\" + Wide(f.path);
        const std::wstring old = target + L"." + stamp + L".old";
        const bool exists = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        bool ok = !exists || MoveFileExW(target.c_str(), old.c_str(), 0);
        if (ok) {
            ok = MoveFileExW((staging + L"\\" + Wide(f.path)).c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
            if (!ok && exists) MoveFileExW(old.c_str(), target.c_str(), 0);
        }
        if (!ok) {
            *error = "cannot replace " + f.path + " (error " + std::to_string(GetLastError()) + ")";
            // Back to what was there.
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                DeleteFileW(it->target.c_str());
                if (it->hadOld) MoveFileExW(it->old.c_str(), it->target.c_str(), 0);
            }
            return false;
        }
        done.push_back({target, old, exists});
    }
    return true;
}

void Install()
{
    ThreadGuard running;
    UpdateInfo info;
    std::string base;
    {
        std::lock_guard lock(g_mutex);
        if (g_view.state != UpdateView::State::Available) return;
        info = g_info;
        base = g_view.url;
        g_view.state = UpdateView::State::Downloading;
        g_view.done = 0;
        g_view.total = 0;
        for (const auto& f : info.files) g_view.total += f.size;
    }
    const std::wstring staging = DataDir() + L"\\update\\" + Wide(info.version);
    EnsureDirectory(staging);
    uint64_t before = 0;
    std::string error;
    for (const UpdateFile& f : info.files) {
        std::vector<uint8_t> data;
        const std::string url = ResolveUrl(base, f.url);
        const bool got = HttpGet(url, static_cast<size_t>(f.size) + 1, &data, &error, [&](uint64_t n) {
            std::lock_guard lock(g_mutex);
            g_view.done = before + n;
        });
        if (got && data.size() != f.size) error = f.path + ": " + std::to_string(data.size()) + " bytes, expected " + std::to_string(f.size);
        else if (got && Sha256Hex(data.data(), data.size()) != f.sha256) error = f.path + " does not match its SHA-256";
        if (!error.empty() || !got) {
            Log("update: %s", error.c_str());
            Set(UpdateView::State::Failed, error);
            return;
        }
        FILE* out = _wfopen((staging + L"\\" + Wide(f.path)).c_str(), L"wb");
        const bool written = out && std::fwrite(data.data(), 1, data.size(), out) == data.size();
        if (out) std::fclose(out);
        if (!written) {
            error = "cannot write " + f.path + " into " + Narrow(staging);
            Log("update: %s", error.c_str());
            Set(UpdateView::State::Failed, error);
            return;
        }
        before += f.size;
    }
    if (!Apply(staging, info, &error)) {
        Log("update: %s; nothing changed", error.c_str());
        Set(UpdateView::State::Failed, error);
        return;
    }
    RemoveDirectoryW(staging.c_str());
    Log("update: SFVR %s installed (%zu files); the next launch runs it", info.version.c_str(), info.files.size());
    Set(UpdateView::State::Ready);
}

// What an earlier update left: the replaced files, which nothing runs now,
// and any staging folder of a download that did not finish.
void CleanUp()
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_gameDir + L"\\*.old").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (DeleteFileW((g_gameDir + L"\\" + fd.cFileName).c_str())) Log("update: removed %s", Narrow(fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const std::wstring update = DataDir() + L"\\update";
    h = FindFirstFileW((update + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::wstring dir = update + L"\\" + fd.cFileName;
            WIN32_FIND_DATAW inner;
            HANDLE g = FindFirstFileW((dir + L"\\*").c_str(), &inner);
            if (g != INVALID_HANDLE_VALUE) {
                do {
                    if (!(inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + inner.cFileName).c_str());
                } while (FindNextFileW(g, &inner));
                FindClose(g);
            }
            RemoveDirectoryW(dir.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void RunInBackground(void (*work)())
{
    if (g_busy.exchange(true)) return;
    std::thread([work] {
        work();
        g_busy = false;
    }).detach();
}

void ApplyUrl(const std::string& url)
{
    bool anyKey = false;
    for (uint8_t b : kUpdatePublicKey) anyKey |= b != 0;
    std::lock_guard lock(g_mutex);
    g_view.url = url;
    if (!anyKey) {
        g_view.state = UpdateView::State::Off;
        g_view.message = "this build has no update signing key";
    }
    else if (url.empty()) {
        g_view.state = UpdateView::State::Off;
        g_view.message = "no update site set ([update] url in sfv_rollback.ini)";
    }
    else if (g_view.state == UpdateView::State::Off) {
        g_view.state = UpdateView::State::Idle;
        g_view.message.clear();
    }
}

} // namespace

void InstallUpdater(const std::wstring& gameDir, const std::string& currentVersion, const std::string& url, bool checkNow)
{
    g_gameDir = gameDir;
    {
        std::lock_guard lock(g_mutex);
        g_view.current = currentVersion;
    }
    g_configured = url.empty() ? kDefaultUpdateUrl : url;
    ApplyUrl(g_configured);
    CleanUp();
    const UpdateView v = UpdateViewNow();
    Log("update: %s", v.state == UpdateView::State::Off ? ("off: " + v.message).c_str() : ("site " + v.url).c_str());
    if (checkNow) UpdateCheck();
}

UpdateView UpdateViewNow()
{
    std::lock_guard lock(g_mutex);
    return g_view;
}

void UpdateCheck()
{
    if (UpdateViewNow().state == UpdateView::State::Off) return;
    RunInBackground(&Check);
}

void UpdateInstall()
{
    if (UpdateViewNow().state != UpdateView::State::Available) return;
    RunInBackground(&Install);
}

void SetUpdateUrl(const std::string& url)
{
    ApplyUrl(url.empty() ? g_configured : url);
    Log("update: site %s", url.empty() ? g_configured.c_str() : url.c_str());
}

} // namespace sfv::mod
