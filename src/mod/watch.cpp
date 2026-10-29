#include "mod/watch.h"

#include "mod/log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace sfv::mod {

namespace {

constexpr int kMaxSites = 32;

constexpr int kStackDepth = 12;

struct Site {
    std::atomic<uintptr_t> rip{0};
    std::atomic<uint64_t> hits{0};
    uintptr_t stack[kStackDepth] = {}; // callers at the first hit (absolute addresses)
};

// The callers of the trapping code, unwound from its context.
void UnwindFrom(CONTEXT c, uintptr_t* out)
{
    for (int i = 0; i < kStackDepth; i++) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
        }
        else {
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        out[i] = static_cast<uintptr_t>(c.Rip);
    }
}

Site g_sites[kMaxSites];
std::atomic<uint64_t> g_dropped{0};
uintptr_t g_imageBase = 0;
PVOID g_handler = nullptr;
std::atomic<uint64_t> g_armed{0}; // DR7 bits we set; 0 once disarmed
uint64_t g_everArmed = 0;        // DR7 bits we ever set

void Record(uintptr_t rip, const CONTEXT& ctx)
{
    for (auto& s : g_sites) {
        uintptr_t cur = s.rip.load();
        if (cur == rip) {
            s.hits++;
            return;
        }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (s.rip.compare_exchange_strong(expected, rip)) {
                UnwindFrom(ctx, s.stack);
                s.hits++;
                return;
            }
            if (expected == rip) {
                s.hits++;
                return;
            }
        }
    }
    g_dropped++;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    // A trap can still arrive after disarming (the thread hit it while we were
    // clearing the registers): swallow it and clear this thread's registers.
    if (g_armed == 0) ctx->Dr7 &= ~g_everArmed;
    // Data breakpoints trap after the write: Rip is the next instruction.
    else Record(static_cast<uintptr_t>(ctx->Rip), *ctx);
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Applies `update` to the debug registers of every other thread.
template <typename F> int ForEachThread(F update)
{
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    int done = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                update(ctx);
                if (SetThreadContext(t, &ctx)) done++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return done;
}

} // namespace

bool ArmWatch(int slot, const void* address, int length, uintptr_t imageBase, bool readsToo)
{
    if (slot < 0 || slot > 3) return false;
    // DR7 length encoding: 1 -> 00, 2 -> 01, 8 -> 10, 4 -> 11. Address must be aligned to it.
    uint64_t len;
    switch (length) {
    case 1: len = 0; break;
    case 2: len = 1; break;
    case 4: len = 3; break;
    case 8: len = 2; break;
    default: return false;
    }
    const auto addr = reinterpret_cast<uintptr_t>(address);
    if (addr % length) {
        Log("watch: %p is not aligned to %d", address, length);
        return false;
    }
    g_imageBase = imageBase;
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, Handler);
    for (auto& s : g_sites) {
        s.rip = 0;
        s.hits = 0;
    }

    const uint64_t enable = 1ull << (slot * 2);
    const uint64_t control = ((readsToo ? 3ull : 1ull) | (len << 2)) << (16 + slot * 4);
    const uint64_t mask = (3ull << (slot * 2)) | (0xFull << (16 + slot * 4));
    const int threads = ForEachThread([&](CONTEXT& ctx) {
        (&ctx.Dr0)[slot] = addr;
        ctx.Dr7 = (ctx.Dr7 & ~mask) | enable | control;
    });
    g_armed |= mask;
    g_everArmed |= mask;
    Log("watch: slot %d on %p (%d bytes, %s), %d threads", slot, address, length, readsToo ? "read/write" : "write",
        threads);
    return threads > 0;
}

void DisarmWatches()
{
    const uint64_t armed = g_armed.exchange(0);
    if (armed) ForEachThread([&](CONTEXT& ctx) { ctx.Dr7 &= ~armed; });
    // The handler stays installed: a thread may already be dispatching a trap.
}

void RemoveWatchHandler()
{
    DisarmWatches();
    if (!g_handler) return;
    // Give in-flight traps time to reach the handler before it goes away.
    Sleep(250);
    RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}

void WatchReport()
{
    int n = 0;
    for (const auto& s : g_sites) {
        const uintptr_t rip = s.rip;
        if (!rip) continue;
        const bool inImage = rip >= g_imageBase && rip < g_imageBase + 0x5000000;
        Log("watch: write before rip %s0x%llX, %llu hits", inImage ? "rva " : "",
            static_cast<unsigned long long>(inImage ? rip - g_imageBase : rip),
            static_cast<unsigned long long>(s.hits.load()));
        std::string callers;
        for (uintptr_t a : s.stack) {
            if (!a) break;
            char buf[24];
            if (a >= g_imageBase && a < g_imageBase + 0x5000000) std::snprintf(buf, sizeof(buf), " %llX", static_cast<unsigned long long>(a - g_imageBase));
            else std::snprintf(buf, sizeof(buf), " ext");
            callers += buf;
        }
        Log("watch:   called from%s", callers.c_str());
        n++;
    }
    Log("watch: %d sites, %llu dropped", n, static_cast<unsigned long long>(g_dropped.load()));
}

} // namespace sfv::mod
