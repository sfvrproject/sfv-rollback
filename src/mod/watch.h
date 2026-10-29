#pragma once
// Hardware write watchpoints, for finding the code that writes a piece of
// state (for example state that differs after a restore).
//
// ArmWatch sets debug register `slot` (0-3) on every thread of the process to
// trap writes to [address, address + length). A vectored exception handler
// counts the instruction that triggered each trap; WatchReport logs them as
// RVAs of the game image.

#include <cstddef>
#include <cstdint>

namespace sfv::mod {

// `readsToo` traps reads as well as writes.
bool ArmWatch(int slot, const void* address, int length, uintptr_t imageBase, bool readsToo = false);
void DisarmWatches();
// Only before unloading the DLL.
void RemoveWatchHandler();
void WatchReport();

} // namespace sfv::mod
