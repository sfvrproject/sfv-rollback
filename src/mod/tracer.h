#pragma once
// Call tracer for reverse engineering: "trace <rva> [<rva> ...]" hooks each
// function start with a stub that counts calls and remembers the last first
// argument (rcx) and return address, then jumps to the original. The stub
// touches no other register and not the stack, so any signature is safe.
// "tracereport" logs the functions that were called since the last report.

#include <cstdint>
#include <vector>

namespace sfv::mod {

void TraceFunctions(uintptr_t imageBase, const std::vector<uint32_t>& rvas);
void TraceReport(uintptr_t imageBase);

} // namespace sfv::mod
