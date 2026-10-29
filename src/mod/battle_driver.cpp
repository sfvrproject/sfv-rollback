#include "mod/battle_driver.h"

#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/stack.h"

#include <MinHook.h>

#include <atomic>
#include <set>
#include <string>
#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace sfv::mod {

namespace {

using RecordFrameFn = uint64_t (*)(uint8_t* inputUnit, void* a2, void* a3, void* a4);

SfvGame* g_game = nullptr;
uintptr_t g_imageBase = 0;
SfvGame::BattleFrameFn g_origBattleFrame = nullptr;
RecordFrameFn g_origRecordFrame = nullptr;
std::atomic<IFrameDriver*> g_driver{nullptr};
bool g_inDriver = false;

// Rose's soul level (her entity's +0x444, a 2..5 she changes during the
// intro and later) is picked by rva 0x3AD7E0 from a random stream that is not
// in the snapshot and differs between processes, so the two games picked
// different levels and her effects followed (netplay-testing.md, finding 19).
// While the picker runs, every draw first reseeds that stream from state that
// is in the snapshot: the battle's tick count and her current level. Both
// games, and a resimulation, then draw the same.
constexpr game::CodeSite kRoseSoulPick{
    "Rose soul level pick", 0x3AD7E0,
    {0x40, 0x55, 0x53, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00}};
// The random stream's draw: state = state * 0x19660D + 0x3C6EF35F at +0x8.
constexpr game::CodeSite kRandomStreamDraw{
    "random stream draw", 0x32B2B0,
    {0x69, 0x41, 0x08, 0x0D, 0x66, 0x19, 0x00, 0x05, 0x5F, 0xF3, 0x6E, 0x3C, 0x89, 0x41, 0x08, 0xC1}};
constexpr uint32_t kBattleTicks = 0x1B8; // int, in the battle frame's state (battleframe::kStateBegin..)
constexpr uint32_t kRoseSoulLevel = 0x444;
constexpr uint32_t kStreamState = 0x8;

using PickFn = uint64_t (*)(uint8_t* entity, void* a2, void* a3, void* a4);
using DrawFn = uint32_t (*)(uint8_t* stream);
PickFn g_origPick = nullptr;
DrawFn g_origDraw = nullptr;
std::atomic<uint8_t*> g_battleFrame{nullptr};
thread_local bool t_picking = false;
thread_local uint32_t t_pickSeed = 0;

uint64_t PickDetour(uint8_t* entity, void* a2, void* a3, void* a4)
{
    HookGuard guard;
    const uint8_t* frame = g_battleFrame;
    const uint32_t ticks = frame ? *reinterpret_cast<const uint32_t*>(frame + kBattleTicks) : 0;
    t_pickSeed = ticks * 0x9E3779B1u ^ *reinterpret_cast<const uint32_t*>(entity + kRoseSoulLevel);
    t_picking = true;
    const uint64_t result = g_origPick(entity, a2, a3, a4);
    t_picking = false;
    return result;
}

std::atomic<int> g_drawLog{0}; // research: draws left to log (`drawlog <count>`)

// Every fighter draws one number per tick from its own stream (rva 0x37D642
// stores it at the fighter object's +0xC50), and moves with a random outcome
// (Dan's) use it. The streams are not in the snapshot and are seeded per
// process, so a replay diverged at the first such move and a peer that
// rolled back drew again while resimulating and diverged from the other
// (netplay-testing.md, finding 28). In driven ticks every draw first sets its
// stream from the match seed, the tick and the draw's place in the tick: the
// same on both machines and in any resimulation.
std::atomic<uint64_t> g_drawSeed{0};

uint32_t TickDrawState()
{
    uint64_t x = g_drawSeed ^ (static_cast<uint64_t>(DrivenTicks()) << 32 | NextTickDraw());
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; // splitmix64's mix
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return static_cast<uint32_t>(x ^ (x >> 31));
}

uint32_t DrawDetour(uint8_t* stream)
{
    HookGuard guard;
    if (t_picking) *reinterpret_cast<uint32_t*>(stream + kStreamState) = t_pickSeed;
    else if (g_inDriver) *reinterpret_cast<uint32_t*>(stream + kStreamState) = TickDrawState();
    if (g_drawLog > 0 && g_inDriver) {
        // The per-tick draw of every fighter (rva 0x37D642) only once per stream.
        static std::set<const void*> perTickSeen;
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_imageBase;
        if (caller != 0x37D642 || perTickSeen.insert(stream).second) {
            g_drawLog--;
            std::string region = "not in the snapshot";
            for (const StateRegion& r : g_game->Regions()) {
                if (stream + kStreamState >= r.ptr && stream + kStreamState < r.ptr + r.size) {
                    region = r.name + "+0x" + std::to_string(stream + kStreamState - r.ptr);
                }
            }
            Log("draw: tick %u, stream %p (state %s) %08X, from rva 0x%llX", DrivenTicks(), static_cast<void*>(stream),
                region.c_str(), *reinterpret_cast<const uint32_t*>(stream + kStreamState), static_cast<unsigned long long>(caller));
        }
    }
    return g_origDraw(stream);
}

void BattleFrameDetour(uint8_t* battleFrame)
{
    HookGuard guard;
    g_battleFrame = battleFrame;
    g_game->OnBattleFrame(battleFrame);
    // The driver's own ticks come back through here via Advance: run them
    // (their time is the driver's, for the frame-drop log).
    if (g_inDriver) {
        g_origBattleFrame(battleFrame);
        return;
    }
    if (IFrameDriver* driver = g_driver) {
        g_inDriver = true;
        bool handled;
        {
            hitch::Scope timed(hitch::Part::NetFrame);
            handled = driver->OnFrame(*g_game);
        }
        g_inDriver = false;
        if (handled) return;
    }
    hitch::Scope timed(hitch::Part::BattleFrame);
    g_origBattleFrame(battleFrame);
}

int g_strayRecords = 0;

uint64_t RecordFrameDetour(uint8_t* inputUnit, void* a2, void* a3, void* a4)
{
    HookGuard guard;
    // With a driver in charge, inputs are recorded only by the ticks it runs.
    // A call from anywhere else would record a local, unsynchronised input
    // and advance the InputUnit's frame counter on one machine only.
    if (g_game->exclusiveTicks && g_driver && !g_inDriver) {
        if (g_strayRecords++ < 3) LogStack("InputUnit::RecordFrame outside a driven tick (skipped)", g_imageBase);
        return 0;
    }
    g_game->OnInputUnit(inputUnit);
    return g_origRecordFrame(inputUnit, a2, a3, a4);
}

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original)
{
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("battle driver: signature mismatch at %s", site.name);
        return false;
    }
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("battle driver: hook %s failed: %s", site.name, MH_StatusToString(st));
        return false;
    }
    return true;
}

} // namespace

void SetMatchDrawSeed(uint64_t seed)
{
    g_drawSeed = seed;
}

void LogGameInputs(int ticks)
{
    if (g_game) g_game->inputLog = ticks;
    Log("inputlog: the game's per-pad values for the next %d ticks, on change", ticks);
}

void LogRandomDraws(int count)
{
    g_drawLog = count;
    Log("draw: logging the next %d draws in driven ticks", count);
}

bool InstallBattleDriver(const GameImage& image, SfvGame* game)
{
    g_game = game;
    SetRegionLookup([game](const void* p) -> std::string {
        for (const StateRegion& r : game->Regions()) {
            if (p >= r.ptr && p < r.ptr + r.size) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s+0x%llx", r.name.c_str(),
                              static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - r.ptr));
                return buf;
            }
        }
        return "not in the snapshot";
    });
    const bool ok = Hook(image, game::kBattleFrame, reinterpret_cast<void*>(&BattleFrameDetour),
                         reinterpret_cast<void**>(&g_origBattleFrame)) &&
                    Hook(image, game::kInputUnitRecord, reinterpret_cast<void*>(&RecordFrameDetour),
                         reinterpret_cast<void**>(&g_origRecordFrame));
    if (!Hook(image, kRoseSoulPick, reinterpret_cast<void*>(&PickDetour), reinterpret_cast<void**>(&g_origPick)) ||
        !Hook(image, kRandomStreamDraw, reinterpret_cast<void*>(&DrawDetour), reinterpret_cast<void**>(&g_origDraw))) {
        Log("battle driver: Rose's soul level and the fighters' random draws stay random per game");
    }
    // SfvGame::Advance runs a tick through the original, not through the hook.
    g_imageBase = image.base;
    game->SetBattleFrameFunction(g_origBattleFrame);
    Log("battle driver %s", ok ? "installed" : "NOT installed");
    return ok;
}

void SetFrameDriver(IFrameDriver* driver) { g_driver = driver; }

IFrameDriver* CurrentFrameDriver() { return g_driver; }

void LogRegionsOfSize(size_t size)
{
    if (!g_game) return;
    for (const StateRegion& r : g_game->Regions()) {
        if (r.size == size) Log("regions: %s at %p", r.name.c_str(), static_cast<void*>(r.ptr));
    }
}

} // namespace sfv::mod
