#pragma once
// The game image: base address and build verification.

#include "mod/sfv_offsets.h"

#include <cstdint>
#include <string>

namespace sfv::mod {

struct GameImage {
    uintptr_t base = 0;
    uint32_t timestamp = 0;

    template <typename T> T At(uint32_t rva) const { return reinterpret_cast<T>(base + rva); }
};

// Finds the game exe in this process and checks its timestamp and every code
// signature. On failure `why` says what did not match.
bool OpenGameImage(GameImage* out, std::string* why);

// True when [p, p + size) is committed, readable memory. Used to validate
// pointers read from game structures before following them.
bool IsReadable(const void* p, size_t size);

} // namespace sfv::mod
