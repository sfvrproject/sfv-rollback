#pragma once
// .sfvr: a match recording that is also a desync report.
//
// Each peer writes its own file. It holds the confirmed inputs of both players
// and, for every frame, one hash per state region taken right after the frame
// was simulated. Two files of the same match can be lined up frame by frame to
// find the first frame and the first regions where the peers disagreed, and one
// file can be played back offline against the game to check that the
// simulation reproduces its own hashes (see verifier.h).

#include "core/game.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sfv {

constexpr uint32_t kReplayFormatVersion = 2; // 2: 32-bit inputs

enum class ReplayEventType : uint16_t {
    Rollback = 1,   // frame: frame loaded, a: frames resimulated
    Desync = 2,     // frame: desynced frame, a: local checksum, b: remote checksum
    Disconnect = 3, // frame: last frame simulated
    Note = 4,       // a, b: free use
};

struct ReplayEvent {
    int32_t frame = 0;
    ReplayEventType type = ReplayEventType::Note;
    uint32_t a = 0;
    uint32_t b = 0;
};

struct RegionInfo {
    std::string name;
    uint64_t size = 0;
    std::vector<uint8_t> ignore; // see StateRegion::ignore
    bool operator==(const RegionInfo&) const = default;
};

// Raw state, regions concatenated in table order.
struct StateDump {
    int32_t frame = 0;
    std::string label;
    std::vector<uint8_t> bytes;
};

struct ReplayFile {
    std::string modVersion;
    std::string gameBuild;
    uint64_t matchId = 0;      // chosen by the host, identical in both peers' files
    int32_t localPlayer = -1;  // -1 when not recorded in a live session
    uint64_t createdUnix = 0;
    uint32_t inputDelay = 0;
    uint32_t predictionWindow = 0;
    // Inputs and hashes up to and including this frame are confirmed. Later
    // frames may still hold predicted remote inputs (a session that ended early).
    int32_t finalThrough = -1;
    MatchSetup setup;
    std::vector<RegionInfo> regions;
    std::vector<uint64_t> initialHashes; // state before frame 0, one per region
    std::vector<FrameInputs> inputs;     // indexed by frame
    std::vector<uint64_t> hashes;        // [frame * regions.size() + region], state after the frame
    std::vector<ReplayEvent> events;
    std::vector<StateDump> dumps;

    int FrameCount() const { return static_cast<int>(inputs.size()); }
    size_t RegionCount() const { return regions.size(); }
    const uint64_t* FrameHashes(int frame) const;
    uint32_t FrameChecksum(int frame) const;
    // Records (or, after a rollback, overwrites) one frame.
    void SetFrame(int frame, const FrameInputs& in, const uint64_t* regionHashes);
    const StateDump* FindDump(int frame) const;
};

std::vector<uint8_t> SerializeReplay(const ReplayFile& replay);
bool DeserializeReplay(const uint8_t* data, size_t len, ReplayFile* out, std::string* error);

bool SaveReplay(const ReplayFile& replay, const std::string& path, std::string* error);
bool LoadReplay(const std::string& path, ReplayFile* out, std::string* error);

} // namespace sfv
