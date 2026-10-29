#pragma once
// Plays a match recording (.sfvr) back in the game: the "replay <file>
// [stress <distance>]" command.
//
// The battle is started from the recording's match setup the way a netplay
// session starts it (a jump with the setup, neutral ticks until the battle's
// objects exist, frozen at frame 0), the state before frame 0 is compared
// with the recording's, then the recorded inputs are played and every
// frame's state is compared, region by region, with the hashes the recording
// holds:
// - straight (no stress): one frame per update, as it was played. A frame
//   that differs means this build does not reproduce the recording (state
//   outside the snapshot that the recording machine had, a different build,
//   or a rollback in the recorded session that did not restore everything);
// - stress <distance>: a GekkoNet stress session fed with the recorded
//   inputs: every frame is also loaded from <distance> frames back and
//   resimulated, and the resimulation must equal the first run. This finds
//   rollback bugs at exactly the recorded situation (characters, moves,
//   round ends), whether or not the recorded session rolled back there.
// The result goes to the log ("replay: PASS / FAIL ..."), with the first
// differing frames and regions.

#include <string>

namespace sfv::mod {

// From any thread; starts with the next battle frame or jump. A relative path
// is taken from the game's reports folder. "stop" ends the running replay.
// watchFrom..watchTo (-1: none): logs watchRegion's first 16 bytes and the
// round's end state for each of those frames (research).
void RequestReplay(const std::string& path, int stressDistance, int watchFrom = -1, int watchTo = -1,
                   const std::string& watchRegion = "");

} // namespace sfv::mod
