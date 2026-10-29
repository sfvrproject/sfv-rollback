#pragma once
// Updates from the project site, asking first. At start the mod reads the
// site's update info (core/update_info.h) and checks its signature; when it
// names a newer version, the panel offers it (Settings, Updates, and a
// toast). If the player agrees, the files are downloaded into SFVR\update,
// checked against their sizes and SHA-256, and swapped in: each file the
// game has (even the loaded loader, xinput1_3.dll) is renamed to
// <name>.<n>.old, the new one moved into its place. The mod runs from a copy
// (proxy/load_mod.h), so the new build takes over at the next launch, which
// also deletes the .old files. The player's ini files are never touched.
//
// The site's address: [update] url in sfv_rollback.ini, else kDefaultUpdateUrl.

#include <cstdint>
#include <string>

namespace sfv::mod {

// The published update info, once the project site is up (empty: no
// updates unless [update] url says where).
constexpr const char* kDefaultUpdateUrl = "";

struct UpdateView {
    enum class State {
        Off,         // no site set, or no signing key in this build
        Idle,        // not checked yet
        Checking,
        UpToDate,
        Available,   // `latest` is newer: the player may install it
        Downloading, // `done` of `total` bytes
        Ready,       // installed: the next launch runs `latest`
        Failed,      // `message` says why; checking again is allowed
    };
    State state = State::Off;
    std::string current;    // this build's version
    std::string latest;     // the site's
    std::string published;
    std::string notes;
    std::string message;    // what failed, or why updates are off
    std::string url;
    uint64_t done = 0, total = 0;
};

// From the mod's start: `gameDir` is where the game's files are (the DLL's
// folder). With `checkNow`, checks in the background at once.
void InstallUpdater(const std::wstring& gameDir, const std::string& currentVersion, const std::string& url, bool checkNow);
UpdateView UpdateViewNow();
// Both run in the background; ignored while a check or download runs.
void UpdateCheck();
void UpdateInstall();
// For tests: another site (empty: the configured one).
void SetUpdateUrl(const std::string& url);

} // namespace sfv::mod
