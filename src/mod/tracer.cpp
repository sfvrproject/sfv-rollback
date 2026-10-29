#include "mod/tracer.h"

#include "mod/log.h"

#include <MinHook.h>
#include <windows.h>

#include <cstring>

namespace sfv::mod {

namespace {

// Per traced function: the stub code followed by its data.
//   lock inc qword [rip+count]
//   mov [rip+lastRcx], rcx
//   push rax; mov rax, [rsp+8]; mov [rip+lastRet], rax; pop rax
//   jmp [rip+original]
struct Stub {
    uint8_t code[64];
    uint64_t count;
    uint64_t lastRcx;
    uint64_t lastRet;
    uint64_t original;
    uint32_t rva;
    uint64_t reported;
};

Stub* g_stubs = nullptr;
size_t g_used = 0;
constexpr size_t kMaxStubs = 512;

int32_t Rel(const Stub& s, size_t codeEnd, const void* field)
{
    return static_cast<int32_t>(reinterpret_cast<const uint8_t*>(field) - (s.code + codeEnd));
}

void Emit(Stub& s)
{
    uint8_t* c = s.code;
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) c[n++] = b;
    };
    auto rel = [&](const void* field) {
        const int32_t d = Rel(s, n + 4, field);
        std::memcpy(c + n, &d, 4);
        n += 4;
    };
    put({0xF0, 0x48, 0xFF, 0x05}); // lock inc qword [rip+count]
    rel(&s.count);
    put({0x48, 0x89, 0x0D}); // mov [rip+lastRcx], rcx
    rel(&s.lastRcx);
    put({0x50});                         // push rax
    put({0x48, 0x8B, 0x44, 0x24, 0x08}); // mov rax, [rsp+8]
    put({0x48, 0x89, 0x05});             // mov [rip+lastRet], rax
    rel(&s.lastRet);
    put({0x58});       // pop rax
    put({0xFF, 0x25}); // jmp [rip+original]
    rel(&s.original);
}

} // namespace

void TraceFunctions(uintptr_t imageBase, const std::vector<uint32_t>& rvas)
{
    if (!g_stubs) {
        // Near the image so the stubs can be reached by MinHook's relative jumps.
        for (uintptr_t want = imageBase - 0x10000000; !g_stubs && want > 0x10000000; want -= 0x1000000) {
            g_stubs = static_cast<Stub*>(VirtualAlloc(reinterpret_cast<void*>(want), sizeof(Stub) * kMaxStubs,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        }
        if (!g_stubs) {
            Log("trace: no memory for stubs");
            return;
        }
    }
    int ok = 0;
    for (uint32_t rva : rvas) {
        if (g_used >= kMaxStubs) break;
        Stub& s = g_stubs[g_used];
        std::memset(&s, 0, sizeof(s));
        s.rva = rva;
        Emit(s);
        void* target = reinterpret_cast<void*>(imageBase + rva);
        void* original = nullptr;
        if (MH_CreateHook(target, s.code, &original) != MH_OK) {
            Log("trace: cannot hook rva 0x%X", rva);
            continue;
        }
        s.original = reinterpret_cast<uint64_t>(original);
        FlushInstructionCache(GetCurrentProcess(), &s, sizeof(s));
        if (MH_EnableHook(target) == MH_OK) {
            g_used++;
            ok++;
        }
    }
    Log("trace: %d functions hooked (%zu total)", ok, g_used);
}

void TraceReport(uintptr_t imageBase)
{
    int n = 0;
    for (size_t i = 0; i < g_used; i++) {
        Stub& s = g_stubs[i];
        const uint64_t count = s.count;
        if (count == s.reported) continue;
        const uint64_t ret = s.lastRet;
        Log("trace: rva 0x%X called %llu times, last rcx %p from rva 0x%llX", s.rva,
            static_cast<unsigned long long>(count - s.reported), reinterpret_cast<void*>(s.lastRcx),
            static_cast<unsigned long long>(ret >= imageBase && ret < imageBase + 0x5000000 ? ret - imageBase : ret));
        s.reported = count;
        n++;
    }
    Log("trace: %d functions called since the last report", n);
}

} // namespace sfv::mod
