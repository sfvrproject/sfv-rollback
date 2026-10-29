#pragma once
// Byte compression for .sfvr chunks: Windows' own XPRESS Huffman codec
// (compressapi.h, in every Windows since 8), so the format needs no library.
// Raw mode: the caller keeps the uncompressed size.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfv {

// False when the codec is unavailable or fails; `out` is then untouched.
bool Compress(const uint8_t* data, size_t size, std::vector<uint8_t>* out);
// `rawSize` is the size before compression; false unless exactly that many
// bytes come back.
bool Decompress(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out);

} // namespace sfv
