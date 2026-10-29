#pragma once
// The seam between the game-agnostic netplay/replay code and a concrete game.
// SfvGame (src/mod) implements it over Street Fighter V's memory; MockGame
// (src/testbed) implements it over a small deterministic fighter so the netcode,
// replay recorder and desync tooling can be tested without the game.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sfv {

constexpr int kNumPlayers = 2;

// One player's input for one frame, after the game's button config is applied.
// For Street Fighter V this is the game's own per-frame input value (what its
// InputUnit records); the bits below are the layout the mock game uses.
using PackedInput = uint32_t;
enum InputBit : PackedInput {
    kUp = 1 << 0,
    kDown = 1 << 1,
    kLeft = 1 << 2,
    kRight = 1 << 3,
    kLP = 1 << 4,
    kMP = 1 << 5,
    kHP = 1 << 6,
    kLK = 1 << 7,
    kMK = 1 << 8,
    kHK = 1 << 9,
    // Start (the game's own bit): skips the intro and the win poses, and
    // pauses during the fight. Netplay passes it on only when both players
    // pressed it during a presentation (flow.h PresentationStart).
    kStart = 1 << 14,
};

struct FrameInputs {
    PackedInput p[kNumPlayers] = {};
    bool operator==(const FrameInputs&) const = default;
};

// Field values are the game's own (battle_setup.h documents them).
struct PlayerSetup {
    uint32_t character = 0; // character code, up to 4 ASCII chars packed little-endian ("KEN", "Z37")
    uint16_t costume = 0;
    uint8_t color = 0;    // 0-based (the menu shows color + 1)
    uint8_t vskill = 0;   // 0 = I, 1 = II, 2 = III
    uint8_t vtrigger = 0; // 0 = I, 1 = II
    uint8_t comment = 0;  // V-Comment: 0 = Auto, n = the n-th comment
    uint8_t variation = 0; // the costume's variant (held button at load in the menus); 0 = none
    bool operator==(const PlayerSetup&) const = default;
};

inline uint32_t PackCode(const char* code)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && code[i]; i++) v |= uint32_t(uint8_t(code[i])) << (8 * i);
    return v;
}

inline std::string UnpackCode(uint32_t v)
{
    std::string s;
    for (int i = 0; i < 4 && (v >> (8 * i)) & 0xFF; i++) s += char((v >> (8 * i)) & 0xFF);
    return s;
}

// Everything both peers must agree on before frame 0. The host's values are
// the source of truth; they travel through Steam lobby data. Anything the match
// setup would otherwise pick at random is derived from `seed`.
struct MatchSetup {
    PlayerSetup players[kNumPlayers];
    uint16_t stage = 0;
    uint8_t stageVariant = 0;
    uint8_t roundsToWin = 2;
    uint16_t timerSeconds = 99;
    uint64_t seed = 0;
    // The session so far: each player's wins and current win streak (what the
    // battle's "Wins" counter and the results screen start from).
    uint16_t wins[kNumPlayers] = {};
    uint16_t streaks[kNumPlayers] = {};
    bool operator==(const MatchSetup&) const = default;
};

// A block of memory that is part of the rollback state. The order of the
// region list is stable for a given game build; replays store it and refuse to
// compare region hashes against a different layout.
//
// `ignore` has one bit per 8-byte word (bit i covers bytes 8i..8i+7). Masked
// words are still saved and restored but left out of hashes and diffs: raw
// game memory holds heap and vtable pointers that differ between machines and
// between runs, and must not count as a desync.
struct StateRegion {
    std::string name;
    uint8_t* ptr = nullptr;
    size_t size = 0;
    std::vector<uint8_t> ignore;
};

class IGame {
public:
    virtual ~IGame() = default;

    // Identifies the game build and adapter version. Stored in replays; replays
    // are only comparable between identical builds.
    virtual std::string BuildId() const = 0;

    // Puts the game into the first frame of the match described by `setup`.
    // Returns false when the adapter cannot do that (yet).
    virtual bool BeginMatch(const MatchSetup& setup) = 0;

    // The live rollback state. May change between matches, not during one.
    virtual const std::vector<StateRegion>& Regions() = 0;

    // Reads the physical controller of the local player.
    virtual PackedInput ReadLocalInput(int player) = 0;

    // Runs exactly one simulation tick. `rollingBack` is true for frames being
    // resimulated after a load; the game should skip audio and effects then.
    virtual void Advance(const FrameInputs& inputs, bool rollingBack) = 0;

    // Called after the region memory has been written back for `frame`, for
    // fix-ups that a plain memcpy cannot do (the game's post-restore callbacks).
    virtual void OnStateLoaded(int frame) { (void)frame; }

    // True once the match's outcome is decided (in the current state): what
    // follows (win screens, menus) need not stay in step, so the session stops
    // comparing states. Must turn true on the same frame on both peers.
    virtual bool OutcomeDecided() { return false; }
};

} // namespace sfv
