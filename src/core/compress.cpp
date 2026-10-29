#include "core/compress.h"

#include <windows.h>

#include <compressapi.h>

namespace sfv {

namespace {

constexpr DWORD kAlgorithm = COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW;

} // namespace

bool Compress(const uint8_t* data, size_t size, std::vector<uint8_t>* out)
{
    COMPRESSOR_HANDLE c = nullptr;
    if (!CreateCompressor(kAlgorithm, nullptr, &c)) return false;
    // XPRESS Huffman never grows its input by more than a little.
    std::vector<uint8_t> buf(size + size / 8 + 1024);
    SIZE_T written = 0;
    const bool ok = ::Compress(c, data, size, buf.data(), buf.size(), &written) != FALSE;
    CloseCompressor(c);
    if (!ok) return false;
    buf.resize(written);
    *out = std::move(buf);
    return true;
}

bool Decompress(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>* out)
{
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(kAlgorithm, nullptr, &d)) return false;
    std::vector<uint8_t> buf(rawSize);
    SIZE_T written = 0;
    const bool ok = ::Decompress(d, data, size, buf.data(), buf.size(), &written) != FALSE && written == rawSize;
    CloseDecompressor(d);
    if (!ok) return false;
    *out = std::move(buf);
    return true;
}

} // namespace sfv
