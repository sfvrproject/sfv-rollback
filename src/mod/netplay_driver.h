#pragma once
// Netplay sessions with match codes.
//
//   host                 opens a room on the Weyvelength server (or, for
//                        testing, a UDP port) and shows its code (panel,
//                        clipboard)
//   join <code>          joins the room behind the code as the other player
//                        (or as a spectator when both seats are taken)
//   spectate <code>      joins it to watch
//
// A session is any number of matches between the same two players. In its
// lobby each player picks a character (shown to the other), the host's rules
// apply, and a match starts once both are ready and both games have both
// characters loaded. For each match both sides jump into a local Versus
// battle with the host's match setup (battle_setup.h), freeze it at frame 0
// until both have it loaded, and let a GekkoNet session drive it
// (battle_driver.h). When the match is decided each player chooses Rematch
// (straight into the next battle) or Character select (back to the lobby);
// leaving ends the session for both. Spectators load the same battles and
// play the host's confirmed inputs (core/spectate.h). Desync reports and the
// match recordings go to the report folder.

#include "core/game.h"
#include "core/lobby.h"
#include "core/mux.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

class SfvGame;

// The public rooms server (the sfvr-server repository): where a fresh
// install hosts and joins unless [weyve] server or the panel's Debug tab
// says otherwise.
inline constexpr const char* kDefaultServer = "159.69.32.141:5555";

struct NetplayOptions {
    bool udp = false;                      // the testing transport (U- codes) instead of Weyvelength rooms
    std::string server = kDefaultServer;    // Weyvelength server, host[:port]
    // The server the panel chose (its Debug tab, or its saved settings), the
    // one its settings file keeps; empty: none. A `server` command (test
    // scripts) changes `server` for this run only, not this.
    std::string savedServer;
    std::vector<std::string> recentServers; // the Debug tab's list, newest first
    std::string name;                      // display name; empty: the Steam persona name
    uint16_t udpPort = 47001;              // UDP: the host listens here; a joiner binds any port
    std::string udpHost = "127.0.0.1";     // UDP: address the host writes into its code
    uint8_t inputDelay = 2;
    uint8_t predictionWindow = 8;
    bool lockstepPresentation = true;      // no prediction (so no rollbacks) outside the fight (flow.h)
    bool lockstepDelay = true;             // there, a local input delay that covers the one-way trip
    bool roundEndRollback = true;          // a round's end may be rolled back (flow.h), instead of held
    bool hasPick = false;                  // this player's own character choice
    PlayerSetup pick;
    uint16_t stage = 0xFFFF;               // host: stage table index (flow.h); 0xFFFF keeps the prepared one
    std::string stageCode;                 // host: as configured; turned into `stage` when a match starts
    bool randomSetup = false;              // host: random characters, settings and stage (testing)
    bool randomCostumes = false;           // with randomSetup: costumes other than the default too
    NetConditions conditions;              // simulated latency/jitter/loss on what this side sends
    uint64_t mashSeed = 0;                 // testing: the local player mashes random inputs
    int mashFrom = 0;                      // testing: ... from this session frame on
    int dumpEvery = 600;                   // full state dumps in match recordings: every N confirmed frames
    int maxDumps = 24;                     // ... at most this many
    bool fullRecordings = false;           // recordings keep every frame's region hashes, not only around desyncs (testing)
    bool skipSaves = true;                 // saves that can never be loaded skip their copy (netplay.h)
    bool verifyLoads = true;               // each load checked against its save's checksum (netplay.h)
    bool menuAfterMatch = true;            // back to the main menu when a session ends or on leave
    bool reconcileStart = true;            // joiner takes the host's values where the frozen states differ (lobby.h)
    bool allowSpectators = true;           // host: others may watch
    bool autoReady = false;                // ready as soon as the lobby is reached (commands, tests)
    bool showOwnFps = false;               // the match HUD also shows this game's frame rate
};

void InstallNetplay(SfvGame* game, const std::wstring& reportDir, const std::string& modVersion,
                    const NetplayOptions& defaults);
// The options the next host/join uses; commands change them.
NetplayOptions& NetOptions();
// F7 hosts, F8 joins with the code on the clipboard (game window focused).
void NetPollHotkeys();
void NetHost(const NetplayOptions& options);
void NetJoin(const std::string& code, const NetplayOptions& options, bool spectate = false);
void NetLeave();
// In the lobby: ready to start (a player), and this player's pick.
void NetSetReady(bool ready);
void NetSetPick(bool hasPick, const PlayerSetup& pick);
// Between matches: this player's input delay for the next match; the host's
// stage for the next matches (a stage code, "random", or "" as prepared).
void NetSetDelay(uint8_t frames);
void NetSetStage(const std::string& code);
// After a match: Rematch or CharacterSelect.
void NetChoose(lobby::Choice choice);
// Testing: leaves a running battle for stage select (pause menu).
void NetPark();
void NetSetConditions(const NetConditions& conditions);
std::string NetStatus();

// Netplay as the overlay shows it: one consistent copy, safe from any thread
// (never blocks: while the driver is being replaced it returns the previous
// copy).
struct NetView {
    enum class Phase { Idle, Connecting, Hosting, Joining, Lobby, Jumping, Loading, Running, PostMatch, Ended };
    Phase phase = Phase::Idle;
    bool host = false;
    bool spectator = false;
    bool udp = false;
    std::string code;     // the match code (hosting or joining)
    std::string status;   // the status line
    std::string result;   // how the last session ended, or the last failure
    std::string server;   // the Weyvelength server
    // The session: both seats (P1 is the host), whose seat is this one's (-1
    // for a spectator), matches finished, spectators watching.
    lobby::Seat seats[kNumPlayers];
    int you = -1;
    int played = 0;
    int spectators = 0;
    unsigned linkPingMs = 0; // round trip of the session's heartbeat
    // How the link to the other side (a spectator's: to the host) travels,
    // from its selected ICE candidate pair; the TURN server's name when one
    // was handed out ("turn.cloudflare.com").
    enum class Path { Unknown, Direct, Relayed };
    Path linkPath = Path::Unknown;
    std::string relayHost;
    uint16_t stageRule = 0xFFFF; // the host's: a stage table index, lobby::kRandomStage or kAnyStage
    bool overResults = false; // the lobby after a match: over its results screen (character select stays there)
    bool haveSetup = false;
    MatchSetup setup;     // what both sides play, once agreed
    // While a match runs (copied by the game thread every frame):
    bool syncing = false; // connected, waiting for the first inputs
    int frame = 0;
    unsigned pingMs = 0;
    float avgPingMs = 0;
    float jitterMs = 0;
    float framesAhead = 0;
    unsigned rollbacks = 0;
    unsigned longestRollback = 0;
    int desyncFrame = -1;
    std::string desyncReport; // where the desync report was saved
    int inputDelay = 0;
    int predictionWindow = 0; // in effect now (0 outside the fight)
    int behind = 0;           // spectator: frames in hand not yet played
};
NetView NetViewNow();
// Whether a battle is running and ticking, and its frame counters (diagnostics).
std::string BattleInfo();

} // namespace sfv::mod
