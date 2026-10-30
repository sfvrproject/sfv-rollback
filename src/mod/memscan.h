#pragma once
// Value scanner over the process's private read-write memory, for locating
// game data from the outside (change a setting in a menu, rescan).
//
//   scan <int32>      new scan for 4-byte-aligned values equal to <int32>
//   rescan <int32>    keep the addresses that now hold <int32>
//   scanshow          log the remaining addresses (up to 64) with context
//   scansmall         new scan for every value in 1..63, remembering each
//   scandelta <d>     keep the addresses whose value changed by exactly <d>
//
// Runs on the command thread; reads go through ReadProcessMemory so memory
// freed meanwhile cannot fault.

#include <cstdint>
#include <vector>

namespace sfv::mod {

void ScanStart(int32_t value);
void ScanNext(int32_t value);
void ScanShow();
void ScanSmall();
// Logs up to 32 addresses holding the 8-byte value (who points at an object).
void FindPointer(uint64_t value);
// Every place (any alignment) holding `bytes` in writable memory, up to 400 (research).
void FindBytes(const std::vector<uint8_t>& bytes);
void ScanDelta(int32_t delta);
// Keeps hits where (hit - fieldOffset) looks like a player record (battle_setup.h).
void ScanRecordFilter(uint32_t fieldOffset);

} // namespace sfv::mod
