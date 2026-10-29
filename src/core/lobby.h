#pragma once
// Messages exchanged on the control channel: joining a session (any number
// of matches between the same two players, watched by up to 8 spectators),
// its lobby, and the loading of each match.
//
//   joiner -> host   Hello         the code's nonce (UDP), build fingerprint,
//                                  wanted role (player or spectator), name
//   host -> joiner   Accept        the role given (a full player seat makes a
//                                  player a spectator) and the host's name
//   host -> joiner   Reject        wrong code, different build, busy, ...
//   player -> host   PlayerState   pick, ready, characters preloaded, choice
//                                  after a match (rematch, character select),
//                                  input delay, frame rate, wired or Wi-Fi
//   host -> all      SessionState  both players' names, picks, ready flags,
//                                  choices, the session score and phase
//   host -> all      Welcome       start a match: its id and setup
//   all -> host      Loaded        battle created and frozen at frame 0, with
//                                  the setup the game actually loaded and the
//                                  checksum of the frozen state
//   host -> spec     Inputs        confirmed inputs of both players, with the
//                                  host's checksum of each frame
//   spec -> host     InputsAck     the next frame the spectator needs
//   all              Bye           leaving
//
// PlayerState and SessionState are sent every 250 ms whatever changed (they
// are the heartbeat too, and carry timestamps for the round trip); Hello and
// Loaded are repeated until answered: the transport may drop any of them.
//
// When the frozen states differ (memory the game has not initialised yet, so
// it holds what each process had there before), the joiner (and every
// spectator) takes the host's values before the match starts:
//
//   host -> joiner   StateHashes   per-region hashes of the frozen state
//   joiner -> host   StateRequest  the host regions it has no equal of
//   host -> joiner   StateData     their bytes, in chunks
//
// and its next Loaded says it is done (`reconciled`).

#include "core/game.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfv::lobby {

constexpr uint32_t kMagic = 0x4E564653; // "SFVN"
constexpr uint16_t kProtocol = 6;
// Message size limits: under Weyvelength's 1024-byte datagrams.
constexpr size_t kHashesPerMessage = 100;
constexpr size_t kRegionsPerRequest = 200;
constexpr size_t kStateChunk = 900;
constexpr size_t kInputsPerMessage = 60;
constexpr size_t kMaxName = 32;
constexpr uint16_t kAnyStage = 0xFFFF;    // whatever the game has prepared
constexpr uint16_t kRandomStage = 0xFFFE; // a random stage per match

enum class Type : uint8_t {
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    Loaded = 4,
    Bye = 5,
    StateHashes = 6,
    StateRequest = 7,
    StateData = 8,
    Accept = 9,
    PlayerState = 10,
    SessionState = 11,
    Inputs = 12,
    InputsAck = 13,
};
enum class RejectReason : uint8_t {
    WrongCode = 1,
    BuildMismatch = 2,
    Busy = 3,
    ProtocolMismatch = 4,
    NoSpectators = 5,
};
enum class Role : uint8_t { Player = 0, Spectator = 1 };
// A player's answer to the post-match menu.
enum class Choice : uint8_t { None = 0, Rematch = 1, CharacterSelect = 2 };
enum class SessionPhase : uint8_t { Lobby = 0, Match = 1, PostMatch = 2 };
// How a player's machine is connected (its default route's adapter).
enum class Link : uint8_t { Unknown = 0, Wired = 1, WiFi = 2 };

// One player as the session sees it.
struct Seat {
    std::string name;
    bool present = false;   // the seat is taken
    bool hasPick = false;
    PlayerSetup pick;
    bool ready = false;
    bool preloaded = false; // both players' characters are in this game's cache
    Choice choice = Choice::None;
    uint16_t wins = 0;      // matches won this session
    uint8_t inputDelay = 0; // the player's own input delay (frames)
    uint8_t fps = 0;        // rendered frames per second, as last measured
    Link link = Link::Unknown;
};

// One confirmed frame for spectators.
struct FrameRecord {
    FrameInputs inputs;
    uint32_t checksum = 0; // the host's; 0: not compared (after the outcome)
};

struct Message {
    Type type = Type::Hello;
    uint16_t protocol = kProtocol;
    // Hello
    uint16_t nonce = 0;
    uint64_t build = 0;
    Role role = Role::Player; // Hello: wanted; Accept: given
    std::string name;         // Hello: the joiner's; Accept: the host's
    // PlayerState (the sender's seat; its name and wins are not used)
    Seat seat;
    // PlayerState, SessionState: heartbeat timestamps (ms, the sender's clock)
    // and the last one received from the other side, with how long ago.
    uint32_t time = 0;
    uint32_t echo = 0;
    uint16_t echoAge = 0;
    // SessionState
    Seat seats[kNumPlayers];
    SessionPhase phase = SessionPhase::Lobby;
    uint16_t played = 0;  // matches finished this session
    uint8_t spectators = 0;
    uint16_t stage = kAnyStage; // the host's stage rule: a stage table index, kRandomStage or kAnyStage
    // Welcome: the agreed setup; Loaded: the setup the game loaded
    MatchSetup setup;
    uint8_t inputDelay = 2; // Welcome: the host's own (each player keeps its own)
    uint8_t predictionWindow = 8;
    // Welcome, Loaded, SessionState (the current or last match), Inputs, InputsAck
    uint64_t matchId = 0;
    // Loaded
    uint32_t stateChecksum = 0;
    bool reconciled = false; // the joiner has taken the host's differing values
    // StateHashes: hashes[i] is region first + i of `total`
    uint32_t first = 0;
    uint32_t total = 0;
    std::vector<uint64_t> hashes;
    // StateRequest: host region indices
    std::vector<uint32_t> regions;
    // StateData: bytes [offset, offset + bytes.size()) of region `region`,
    // which is `total` bytes long
    uint32_t region = 0;
    uint32_t offset = 0;
    std::vector<uint8_t> bytes;
    // Inputs: frames first .. first + frames.size() - 1; InputsAck: `first`
    // is the next frame wanted
    std::vector<FrameRecord> frames;
    // Reject
    RejectReason reason = RejectReason::WrongCode;
};

std::vector<uint8_t> Encode(const Message& m);
std::optional<Message> Decode(const uint8_t* data, size_t len);

} // namespace sfv::lobby
