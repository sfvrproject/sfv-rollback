#include "mod/image.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace sfv::mod {

bool OpenGameImage(GameImage* out, std::string* why)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

    out->base = base;
    out->timestamp = nt->FileHeader.TimeDateStamp;
    if (out->timestamp != game::kExeTimestamp) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "exe timestamp %08X, expected %08X (different game build?)", out->timestamp,
                      game::kExeTimestamp);
        *why = buf;
        return false;
    }

    for (const game::CodeSite* site : game::kAllSites) {
        const void* code = reinterpret_cast<const void*>(base + site->rva);
        if (!IsReadable(code, sizeof(site->bytes)) || std::memcmp(code, site->bytes, sizeof(site->bytes)) != 0) {
            *why = std::string("signature mismatch at ") + site->name +
                   " (code not decrypted yet, already hooked by another mod, or a different build)";
            return false;
        }
    }
    return true;
}

bool IsReadable(const void* p, size_t size)
{
    auto addr = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = addr + size;
    if (!p || end < addr) return false;

    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & readable)) return false;
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

} // namespace sfv::mod
