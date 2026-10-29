#include "mod/memscan.h"

#include "mod/log.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace sfv::mod {

namespace {

std::vector<uintptr_t> g_hits;
std::vector<int32_t> g_values; // scansmall/scandelta: the value at each hit
constexpr size_t kMaxHits = 64u << 20;

bool ReadSafe(uintptr_t address, void* out, size_t size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &got) &&
           got == size;
}

} // namespace

void ScanStart(int32_t value)
{
    g_hits.clear();
    g_values.clear();
    std::vector<uint8_t> buffer(1u << 20);
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    size_t scanned = 0;
    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t chunk = base; chunk < end && g_hits.size() < kMaxHits; chunk += buffer.size()) {
                const size_t n = static_cast<size_t>(std::min<uintptr_t>(buffer.size(), end - chunk));
                if (!ReadSafe(chunk, buffer.data(), n)) continue;
                scanned += n;
                for (size_t i = 0; i + 4 <= n; i += 4) {
                    int32_t v;
                    std::memcpy(&v, &buffer[i], 4);
                    if (v == value) g_hits.push_back(chunk + i);
                }
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    Log("scan %d: %zu hits in %zu MB", value, g_hits.size(), scanned >> 20);
}

void ScanSmall()
{
    g_hits.clear();
    g_values.clear();
    std::vector<uint8_t> buffer(1u << 20);
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t chunk = base; chunk < end && g_hits.size() < kMaxHits; chunk += buffer.size()) {
                const size_t n = static_cast<size_t>(std::min<uintptr_t>(buffer.size(), end - chunk));
                if (!ReadSafe(chunk, buffer.data(), n)) continue;
                for (size_t i = 0; i + 4 <= n; i += 4) {
                    int32_t v;
                    std::memcpy(&v, &buffer[i], 4);
                    if (v >= 1 && v < 64) {
                        g_hits.push_back(chunk + i);
                        g_values.push_back(v);
                    }
                }
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    Log("scansmall: %zu hits", g_hits.size());
}

void FindPointer(uint64_t value)
{
    std::vector<uint8_t> buffer(1u << 20);
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    int hits = 0;
    while (hits < 32 && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
            for (uintptr_t chunk = base; chunk < end && hits < 32; chunk += buffer.size()) {
                const size_t n = static_cast<size_t>(std::min<uintptr_t>(buffer.size(), end - chunk));
                if (!ReadSafe(chunk, buffer.data(), n)) continue;
                for (size_t i = 0; i + 8 <= n && hits < 32; i += 8) {
                    uint64_t v;
                    std::memcpy(&v, &buffer[i], 8);
                    if (v == value) {
                        Log("findptr %llx: at %p", static_cast<unsigned long long>(value), reinterpret_cast<void*>(chunk + i));
                        hits++;
                    }
                }
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    Log("findptr: %d hits", hits);
}

void ScanDelta(int32_t delta)
{
    if (g_values.size() != g_hits.size()) {
        Log("scandelta needs scansmall first");
        return;
    }
    size_t kept = 0;
    for (size_t i = 0; i < g_hits.size(); i++) {
        int32_t v;
        if (!ReadSafe(g_hits[i], &v, 4) || v != g_values[i] + delta) continue;
        g_hits[kept] = g_hits[i];
        g_values[kept] = v;
        kept++;
    }
    g_hits.resize(kept);
    g_values.resize(kept);
    Log("scandelta %d: %zu hits left", delta, kept);
}

void ScanNext(int32_t value)
{
    g_values.clear();
    std::vector<uintptr_t> kept;
    for (uintptr_t a : g_hits) {
        int32_t v;
        if (ReadSafe(a, &v, 4) && v == value) kept.push_back(a);
    }
    g_hits.swap(kept);
    Log("rescan %d: %zu hits left", value, g_hits.size());
}

void ScanRecordFilter(uint32_t fieldOffset)
{
    std::vector<uintptr_t> kept;
    for (uintptr_t a : g_hits) {
        uint8_t r[0x100];
        if (!ReadSafe(a - fieldOffset, r, sizeof(r))) continue;
        auto u32 = [&](uint32_t off) { uint32_t v; std::memcpy(&v, r + off, 4); return v; };
        if (u32(0xE4) <= 2 && u32(0xDC) <= 2 && u32(0xE0) <= 2 && u32(0xB0) < 64 && u32(0xB4) < 64) kept.push_back(a);
    }
    g_hits.swap(kept);
    Log("scanrecord +0x%x: %zu hits left", fieldOffset, g_hits.size());
    for (size_t i = 0; i < g_hits.size() && i < 16; i++) {
        uint32_t f[8];
        ReadSafe(g_hits[i] - fieldOffset + 0x90, f, sizeof(f));
        uint32_t g[6];
        ReadSafe(g_hits[i] - fieldOffset + 0xB0, g, 8);
        ReadSafe(g_hits[i] - fieldOffset + 0xD4, g + 2, 16);
        Log("  record %p: code %08x %08x %08x ver %u costume %u color %u comment %u var %u vtrig %u ca %u",
            reinterpret_cast<void*>(g_hits[i] - fieldOffset), f[0], f[1], f[2], f[3], g[0], g[1], g[2], g[3], g[4], g[5]);
    }
}

void ScanShow()
{
    size_t shown = 0;
    for (uintptr_t a : g_hits) {
        if (shown++ >= 64) break;
        int32_t around[8] = {};
        ReadSafe(a - 16, around, sizeof(around));
        Log("  %p: %d %d %d %d [%d] %d %d %d", reinterpret_cast<void*>(a), around[0], around[1], around[2], around[3],
            around[4], around[5], around[6], around[7]);
    }
    Log("scanshow: %zu hits", g_hits.size());
}

} // namespace sfv::mod
