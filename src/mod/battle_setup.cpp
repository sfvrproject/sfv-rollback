#include "mod/battle_setup.h"

#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/stack.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace sfv::mod {

namespace {

namespace bd = battledata;

constexpr uint32_t kEngineGlobal = 0x3D878C8;
constexpr uint32_t kGameInstance = 0x1E0;
constexpr uint32_t kGetBattleSettingSlot = 0x238;
constexpr uint32_t kBattleDataPtr = 0x30;

// BattleSetting::CheckPlayers(this): once, when character select ends.
constexpr game::CodeSite kCheckPlayers{
    "BattleSetting::CheckPlayers", 0x7B33B0,
    {0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xF9, 0xE8, 0xD0, 0xE2, 0xD6, 0xFF}};
// BattleActor::ReadSettings(this): when the battle is created.
constexpr game::CodeSite kReadSettings{
    "BattleActor::ReadSettings", 0x6A45E0,
    {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48}};

GameImage g_image;
uint64_t (*g_origCheckPlayers)(uint8_t*, void*, void*, void*) = nullptr;
uint64_t (*g_origReadSettings)(uint8_t*, void*, void*, void*) = nullptr;
std::mutex g_mutex;
bool g_pending = false;
MatchSetup g_setup;
// Research (`uniqueparam`): KWBattlePlayerSetting::SetUniqueParam values
// (+0x128, 8 ints) written with the next setups; -1 leaves one alone.
int g_uniqueParams[2][8] = {{-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1}};
std::atomic<int> g_setups{0};
std::atomic<int> g_creations{0};

template <typename T> T Get(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }
template <typename T> void Put(uint8_t* p, uint32_t off, T v) { *reinterpret_cast<T*>(p + off) = v; }

void ApplyPending(const char* when)
{
    std::lock_guard lock(g_mutex);
    if (!g_pending) return;
    if (ApplyMatchSetup(g_setup)) Log("battle setup applied %s: %s", when, DescribeSetup(g_setup).c_str());
}

uint64_t CheckPlayersDetour(uint8_t* self, void* a2, void* a3, void* a4)
{
    HookGuard guard;
    g_setups++;
    LogStack("CheckPlayers", reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    ApplyPending("at character select end");
    return g_origCheckPlayers(self, a2, a3, a4);
}

uint64_t ReadSettingsDetour(uint8_t* self, void* a2, void* a3, void* a4)
{
    HookGuard guard;
    g_creations++;
    LogStack("ReadSettings", reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    ApplyPending("at battle creation");
    return g_origReadSettings(self, a2, a3, a4);
}

bool Hook(const GameImage& image, const game::CodeSite& site, void* detour, void** original)
{
    void* target = image.At<void*>(site.rva);
    if (std::memcmp(target, site.bytes, sizeof(site.bytes)) != 0) {
        Log("battle setup: signature mismatch at %s", site.name);
        return false;
    }
    return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

} // namespace

bool InstallBattleSetup(const GameImage& image)
{
    g_image = image;
    const bool ok = Hook(image, kCheckPlayers, reinterpret_cast<void*>(&CheckPlayersDetour),
                         reinterpret_cast<void**>(&g_origCheckPlayers)) &&
                    Hook(image, kReadSettings, reinterpret_cast<void*>(&ReadSettingsDetour),
                         reinterpret_cast<void**>(&g_origReadSettings));
    Log("battle setup hooks %s", ok ? "installed" : "NOT installed");
    return ok;
}

uint8_t* BattleData()
{
    if (!g_image.base) return nullptr;
    auto* engine = Get<uint8_t*>(reinterpret_cast<uint8_t*>(g_image.base), kEngineGlobal);
    if (!IsReadable(engine, kGameInstance + 8)) return nullptr;
    auto* instance = Get<uint8_t*>(engine, kGameInstance);
    if (!IsReadable(instance, 8)) return nullptr;
    auto* vtable = Get<uint8_t*>(instance, 0);
    if (!IsReadable(vtable, kGetBattleSettingSlot + 8)) return nullptr;
    const auto fn = Get<uintptr_t>(vtable, kGetBattleSettingSlot);
    if (fn < g_image.base || fn >= g_image.base + 0x5000000) return nullptr;
    auto* setting = reinterpret_cast<uint8_t* (*)(void*)>(fn)(instance);
    if (!IsReadable(setting, kBattleDataPtr + 8)) return nullptr;
    auto* data = Get<uint8_t*>(setting, kBattleDataPtr);
    return IsReadable(data, 2 * bd::kRecordStride) ? data : nullptr;
}

bool ReadMatchSetup(MatchSetup* out)
{
    uint8_t* data = BattleData();
    if (!data) return false;
    MatchSetup s;
    for (int p = 0; p < kNumPlayers; p++) {
        const uint8_t* r = data + p * bd::kRecordStride;
        char code[5] = {};
        std::memcpy(code, r + bd::kCharaCode, 4);
        s.players[p].character = PackCode(code);
        s.players[p].costume = static_cast<uint16_t>(Get<int32_t>(r, bd::kCostume));
        s.players[p].color = static_cast<uint8_t>(Get<int32_t>(r, bd::kColor));
        const int32_t comment = Get<int32_t>(r, bd::kComment);
        s.players[p].comment = static_cast<uint8_t>(comment < 0 ? 0 : comment + 1);
        s.players[p].vskill = static_cast<uint8_t>(Get<int32_t>(r, bd::kVSkill));
        s.players[p].vtrigger = static_cast<uint8_t>(Get<int32_t>(r, bd::kVTrigger));
        uint8_t variation = Get<int32_t>(r, bd::kUseDressState) && Get<int32_t>(r, bd::kDressStates) ? kVariantDress : 0;
        if ((std::strcmp(code, "URN") == 0 && Get<int32_t>(r, bd::kUniqueParams + 4 * 2) == 2) ||
            (std::strcmp(code, "Z41") == 0 && Get<int32_t>(r, bd::kUniqueParams) == 1)) {
            variation |= kVariantSpecial;
        }
        s.players[p].variation = variation;
    }
    s.stage = static_cast<uint16_t>(Get<int32_t>(data, bd::kStage));
    *out = s;
    return true;
}

bool ApplyMatchSetup(const MatchSetup& setup)
{
    uint8_t* data = BattleData();
    if (!data) {
        Log("battle setup: no battle data to write to");
        return false;
    }
    for (int p = 0; p < kNumPlayers; p++) {
        uint8_t* r = data + p * bd::kRecordStride;
        const PlayerSetup& ps = setup.players[p];
        if (ps.character) {
            // ASCII then UTF-16 copy of the same code, both NUL padded.
            const std::string code = UnpackCode(ps.character);
            uint8_t field[12] = {};
            for (size_t i = 0; i < code.size() && i < 3; i++) {
                field[i] = static_cast<uint8_t>(code[i]);
                field[4 + 2 * i] = static_cast<uint8_t>(code[i]);
            }
            std::memcpy(r + bd::kCharaCode, field, sizeof(field));
        }
        // The game keeps the version in the record when the new character
        // has it too (rva 0x56B2E0), so it would depend on who played
        // before: -1 makes it resolve to the character's latest.
        Put<int32_t>(r, bd::kCharaVersion, -1);
        Put<int32_t>(r, bd::kCostume, ps.costume);
        Put<int32_t>(r, bd::kColor, ps.color);
        Put<int32_t>(r, bd::kComment, ps.comment == 0 ? -1 : ps.comment - 1);
        Put<int32_t>(r, bd::kVSkill, ps.vskill);
        Put<int32_t>(r, bd::kVTrigger, ps.vtrigger);
        // The costume's variant (the menus' held buttons at load): dress
        // state 1 in every round, and the character's special look, a
        // unique param (flow.h kVariantSpecial).
        const int dress = ps.variation & kVariantDress ? 1 : 0;
        for (int round = 0; round < 7; round++) Put<int32_t>(r, bd::kDressStates + 4 * round, dress);
        Put<int32_t>(r, bd::kUseDressState, dress);
        const bool special = ps.variation & kVariantSpecial;
        const std::string chara(reinterpret_cast<const char*>(r + bd::kCharaCode), 3);
        if (chara == "URN") Put<int32_t>(r, bd::kUniqueParams + 4 * 2, special ? 2 : 0);
        if (chara == "Z41") Put<int32_t>(r, bd::kUniqueParams, special ? 1 : 0);
        for (int k = 0; k < 8; k++) {
            if (g_uniqueParams[p][k] >= 0) Put<int32_t>(r, bd::kUniqueParams + 4 * k, g_uniqueParams[p][k]);
        }
        Put<int32_t>(r, bd::kWins, setup.wins[p]);
        Put<int32_t>(r, bd::kWinStreak, setup.streaks[p]);
        Put<int32_t>(r, bd::kCtrlType, 0);
        Put<int32_t>(r, bd::kPadId, 0);
    }
    // A battle entered straight from another (flow.h JumpToBattle) gets its
    // data from defaults that are not local Versus (infinite time, P2 a CPU).
    Put<int32_t>(data, bd::kMode, 1);
    Put<int32_t>(data, bd::kTimer, 5940);
    Put<int32_t>(data, bd::kVersusFlag, 1);
    // The level itself comes from the stage request (flow.h SetPendingStage);
    // the id here does not change it. But a battle entered from another
    // battle's results kept a stale id (LDN after CHN), and the battle reads
    // its stage id from here (music, stage name): keep the two the same.
    if (setup.stage != kKeepStage) Put<int32_t>(data, bd::kStage, setup.stage);
    return true;
}

void SetUniqueParamOverride(int player, int index, int value)
{
    if (player >= 0 && player < 2 && index >= 0 && index < 8) g_uniqueParams[player][index] = value;
}

void SetPendingSetup(const MatchSetup& setup)
{
    std::lock_guard lock(g_mutex);
    g_setup = setup;
    g_pending = true;
}

bool HasPendingSetup()
{
    std::lock_guard lock(g_mutex);
    return g_pending;
}

void ClearPendingSetup()
{
    std::lock_guard lock(g_mutex);
    g_pending = false;
}

int BattleSetupCount() { return g_setups; }

int BattleCreationCount() { return g_creations; }

std::string DescribeSetup(const MatchSetup& s)
{
    std::string out;
    char buf[160];
    for (int p = 0; p < kNumPlayers; p++) {
        const PlayerSetup& ps = s.players[p];
        std::snprintf(buf, sizeof(buf), "P%d %s costume %u color %u comment %u V-Skill %u V-Trigger %u%s | ", p + 1,
                      UnpackCode(ps.character).c_str(), ps.costume, ps.color + 1, ps.comment, ps.vskill + 1,
                      ps.vtrigger + 1, ps.variation ? (" " + VariantLabel(UnpackCode(ps.character), ps.variation)).c_str() : "");
        out += buf;
    }
    if (s.stage == kKeepStage) return out + "stage as prepared";
    std::snprintf(buf, sizeof(buf), "stage %u %s", s.stage, StageCode(s.stage).c_str());
    return out + buf;
}

void LogBattleSetup(const char* when)
{
    MatchSetup s;
    if (!ReadMatchSetup(&s)) {
        Log("battle setup %s: not available", when);
        return;
    }
    const uint8_t* data = BattleData();
    Log("battle setup %s: %s (rounds %d, timer %d, versions %d/%d)", when, DescribeSetup(s).c_str(),
        Get<int32_t>(data, bd::kRounds), Get<int32_t>(data, bd::kTimer), Get<int32_t>(data, bd::kCharaVersion),
        Get<int32_t>(data + bd::kRecordStride, bd::kCharaVersion));
}

} // namespace sfv::mod
