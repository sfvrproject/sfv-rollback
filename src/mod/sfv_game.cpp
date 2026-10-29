#include "mod/sfv_game.h"

#include "core/state.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/ue4.h"
#include "mod/watch.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace sfv::mod {

namespace {

namespace mgr = game::manager;

template <typename T> T Read(const uint8_t* base, uint32_t offset)
{
    return *reinterpret_cast<const T*>(base + offset);
}

constexpr size_t kMaxRegions = 100000;
constexpr size_t kMaxGroups = 64;

// Sorted committed address ranges, for cheap "is this a pointer" checks.
std::vector<std::pair<uintptr_t, uintptr_t>> CommittedRanges()
{
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) {
        const auto start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = start + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            if (!out.empty() && out.back().second == start) out.back().second = end;
            else out.emplace_back(start, end);
        }
        if (end <= addr) break;
        addr = end;
    }
    return out;
}

bool InRanges(const std::vector<std::pair<uintptr_t, uintptr_t>>& ranges, uint64_t v)
{
    auto it = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(static_cast<uintptr_t>(v), UINTPTR_MAX));
    if (it == ranges.begin()) return false;
    --it;
    return v >= it->first && v < it->second;
}

// Address ranges seen in use (committed or reserved) and 64 KB granules seen
// free; the free ones are forgotten every few seconds as the heaps grow.
std::mutex g_addressMutex;
std::vector<std::pair<uintptr_t, uintptr_t>> g_addressRanges;
std::unordered_set<uint64_t> g_freeGranules;
ULONGLONG g_freeGranulesSince = 0;

} // namespace

bool LooksLikeAddress(uint64_t v)
{
    // Below 4 GB are mostly plain integers; nothing of the game's lives there.
    if (v < 0x100000000ull || v >= 0x800000000000ull) return false;
    std::lock_guard lock(g_addressMutex);
    if (InRanges(g_addressRanges, v)) return true;
    // Free granules are forgotten quickly: a pointer to memory allocated since
    // must not stay "not an address" on one machine only.
    const ULONGLONG now = GetTickCount64();
    if (now - g_freeGranulesSince > 250) {
        g_freeGranules.clear();
        g_freeGranulesSince = now;
    }
    if (g_freeGranules.count(v >> 16)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof(mbi)) || mbi.State == MEM_FREE) {
        g_freeGranules.insert(v >> 16);
        return false;
    }
    // Remember the whole reservation, not just this part of it: heaps commit
    // more of their reservations as they grow.
    const auto base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    MEMORY_BASIC_INFORMATION next;
    while (VirtualQuery(reinterpret_cast<const void*>(end), &next, sizeof(next)) && next.AllocationBase == mbi.AllocationBase &&
           next.State != MEM_FREE) {
        end = reinterpret_cast<uintptr_t>(next.BaseAddress) + next.RegionSize;
    }
    const auto range = std::make_pair(std::min(base, reinterpret_cast<uintptr_t>(mbi.BaseAddress)), end);
    g_addressRanges.insert(std::upper_bound(g_addressRanges.begin(), g_addressRanges.end(), range), range);
    return true;
}

// ---------------------------------------------------------------------------
// StateMap

bool StateMap::Load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    bool ok = true;
    auto get = [&](void* p, size_t n) { ok = ok && std::fread(p, 1, n, f) == n; };

    uint32_t magic = 0, version = 0, count = 0;
    get(&magic, 4);
    get(&version, 4);
    get(&count, 4);
    ok = ok && magic == 0x4D564653 /* SFVM */ && version == 1 && count < kMaxRegions;
    regions.clear();
    for (uint32_t i = 0; ok && i < count; i++) {
        Region r;
        uint32_t len = 0;
        get(&r.size, 8);
        get(&len, 4);
        ok = ok && len <= (r.size + 63) / 64;
        if (!ok) break;
        r.ignore.resize(len);
        get(r.ignore.data(), len);
        regions.push_back(std::move(r));
    }
    std::fclose(f);
    if (!ok) regions.clear();
    return ok;
}

bool StateMap::Save(const std::wstring& path) const
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const uint32_t header[3] = {0x4D564653, 1, static_cast<uint32_t>(regions.size())};
    std::fwrite(header, 4, 3, f);
    for (const auto& r : regions) {
        const uint32_t len = static_cast<uint32_t>(r.ignore.size());
        std::fwrite(&r.size, 8, 1, f);
        std::fwrite(&len, 4, 1, f);
        std::fwrite(r.ignore.data(), 1, len, f);
    }
    return std::fclose(f) == 0;
}

uint64_t StateMap::Fingerprint() const
{
    uint64_t h = regions.size();
    for (const auto& r : regions) {
        h = Hash64(&r.size, sizeof(r.size), h);
        h = Hash64(r.ignore.data(), r.ignore.size(), h);
    }
    return h;
}

// ---------------------------------------------------------------------------
// SfvGame

void SfvGame::OnInputUnit(uint8_t* inputUnit)
{
    lastInputMs_ = GetTickCount64();
    if (inputUnit != inputUnit_) {
        inputUnit_ = inputUnit;
        Log("InputUnit at %p, %d pads, local mask %x", inputUnit, Read<int32_t>(inputUnit, game::inputunit::kPadCount),
            Read<uint32_t>(inputUnit, game::inputunit::kLocalMask));
    }
    // The values the game computed from the pads for this tick; then, when
    // driving the tick, what the players actually play with.
    auto* values = reinterpret_cast<uint32_t*>(inputUnit + game::inputunit::kValues);
    const int pads = std::clamp(Read<int32_t>(inputUnit, game::inputunit::kPadCount), 0, 4);
    for (int i = 0; i < pads; i++) sampled_[i] = values[i];
    // Research (`inputlog <ticks>`): what the game computed per pad, on change.
    if (inputLog > 0) {
        inputLog--;
        static uint32_t last[4] = {};
        if (std::memcmp(last, sampled_, sizeof(last)) != 0) {
            std::memcpy(last, sampled_, sizeof(last));
            Log("inputlog: pads %d, values %08X %08X %08X %08X", pads, sampled_[0], sampled_[1], sampled_[2], sampled_[3]);
        }
    }
    if (injecting_) {
        values[0] = inject_[0];
        values[1] = inject_[1];
    }
}

void SfvGame::OnRollbackUnitTick(uint8_t* unit)
{
    if (unit != unit_) {
        unit_ = unit;
        ResetRoundFlow();
        Log("RollbackUnit at %p (new battle)", unit);
    }
}

namespace {

// watchregion: a write watch on a region of the next battle, armed as soon as
// its region list exists (a region's address is only known then).
std::mutex g_regionWatchMutex;
std::string g_regionWatchName;
size_t g_regionWatchOffset = 0;
int g_regionWatchLength = 0;

} // namespace

void WatchRegionOfNextBattle(const std::string& name, size_t offset, int length)
{
    std::lock_guard lock(g_regionWatchMutex);
    g_regionWatchName = name;
    g_regionWatchOffset = offset;
    g_regionWatchLength = length;
    Log("watch: armed for region %s +0x%zx (%d bytes) in the next region list", name.c_str(), offset, length);
}

// A name ending in ".*" (a pool: which slot the game takes is not known in
// advance) is armed by CheckChangeWatch instead, on the first region of that
// prefix whose watched bytes change.
bool IsPoolWatch() { return g_regionWatchName.size() > 2 && g_regionWatchName.compare(g_regionWatchName.size() - 2, 2, ".*") == 0; }

std::map<const uint8_t*, std::vector<uint8_t>> g_poolWatchSeen;

void CheckChangeWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase)
{
    std::lock_guard lock(g_regionWatchMutex);
    if (g_regionWatchName.empty() || !IsPoolWatch()) return;
    const std::string prefix = g_regionWatchName.substr(0, g_regionWatchName.size() - 1); // "s44."
    for (const StateRegion& r : regions) {
        if (r.name.compare(0, prefix.size(), prefix) != 0 || g_regionWatchOffset + g_regionWatchLength > r.size) continue;
        const uint8_t* p = r.ptr + g_regionWatchOffset;
        auto& seen = g_poolWatchSeen[r.ptr];
        const bool changed = !seen.empty() && std::memcmp(seen.data(), p, seen.size()) != 0;
        seen.assign(p, p + g_regionWatchLength);
        if (!changed) continue;
        // From another thread: arming sets every thread's debug registers,
        // and the game thread (this one) cannot set its own while it runs.
        const uint8_t* at = r.ptr + g_regionWatchOffset;
        const int length = g_regionWatchLength;
        std::thread([at, length, imageBase] {
            ThreadGuard running;
            int slot = 0;
            for (int done = 0; done < length && slot < 4; done += 8, slot++) {
                ArmWatch(slot, at + done, std::min(8, length - done), imageBase);
            }
        }).detach();
        Log("watch: region %s at %p changed at +0x%zx; watching it", r.name.c_str(), static_cast<void*>(r.ptr),
            g_regionWatchOffset);
        g_regionWatchName.clear();
        g_poolWatchSeen.clear();
        return;
    }
}

void ArmPendingRegionWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase)
{
    std::lock_guard lock(g_regionWatchMutex);
    if (g_regionWatchName.empty() || IsPoolWatch()) return;
    for (const StateRegion& r : regions) {
        if (r.name != g_regionWatchName || g_regionWatchOffset >= r.size) continue;
        // Debug registers watch up to 8 bytes each: 16 bytes take two.
        int slot = 0;
        for (int done = 0; done < g_regionWatchLength && slot < 4; done += 8, slot++) {
            ArmWatch(slot, r.ptr + g_regionWatchOffset + done, std::min(8, g_regionWatchLength - done), imageBase);
        }
        Log("watch: region %s at %p, watching +0x%zx", r.name.c_str(), static_cast<void*>(r.ptr), g_regionWatchOffset);
        g_regionWatchName.clear();
        return;
    }
}

uint8_t* SfvGame::Manager() const
{
    if (!unit_) return nullptr;
    using GetManagerFn = uint8_t* (*)(void*);
    auto getManager = image_.At<GetManagerFn>(game::kUnitGetManager.rva);
    uint8_t* m = getManager(unit_ + game::unit::kManagerContext);
    return IsReadable(m, mgr::kGroupsEnd + 8) ? m : nullptr;
}

void SfvGame::FindPresentationState()
{
    presentation_.clear();
    if (!ue4::EnsureObjects(image_.base)) return;
    // The current battle's objects: the newest (highest numbered) instances
    // in the persistent level; older battles' may not be collected yet.
    struct Found {
        uint8_t* object;
        int number;
    };
    std::vector<Found> pawns, targets;
    ue4::ForEachObject([&](const void* object) {
        const std::string cls = ue4::ObjectName(ue4::ClassOf(object));
        const bool pawn = cls == "KBP_BattlePawn_C";
        if (!pawn && cls != "KBP_BattleViewTarget_C") return;
        if (ue4::ObjectName(ue4::OuterOf(object)) != "PersistentLevel") return;
        const std::string name = ue4::ObjectName(object);
        const size_t us = name.rfind('_');
        const int number = us == std::string::npos ? -1 : std::atoi(name.c_str() + us + 1);
        (pawn ? pawns : targets).push_back({static_cast<uint8_t*>(const_cast<void*>(object)), number});
    });
    auto newest = [](std::vector<Found>& v) {
        std::sort(v.begin(), v.end(), [](const Found& a, const Found& b) { return a.number > b.number; });
    };
    newest(pawns);
    newest(targets);
    for (size_t i = 0; i < pawns.size() && i < 2; i++) {
        presentation_.push_back({"pawn.clock", pawns[i].object + game::presentation::kPawnClock, 8});
    }
    if (!targets.empty()) presentation_.push_back({"viewtarget.clock", targets[0].object + game::presentation::kViewTargetClock, 8});
    Log("presentation state: %zu pawns, %zu view targets; %zu regions", pawns.size(), targets.size(), presentation_.size());
}

uint8_t* SfvGame::SingletonTicks() const
{
    auto* engine = *image_.At<uint8_t* const*>(game::singleton::kEngineGlobal);
    if (!engine || !IsReadable(engine + game::singleton::kSingleton, 8)) return nullptr;
    uint8_t* singleton = *reinterpret_cast<uint8_t* const*>(engine + game::singleton::kSingleton);
    return singleton && IsReadable(singleton + game::singleton::kTicks, game::singleton::kTicksSize)
               ? singleton + game::singleton::kTicks
               : nullptr;
}

bool SfvGame::RefreshRegions()
{
    // The game's table, in its order, plus our extra regions; only rebuilt
    // when that changes (a new battle).
    std::vector<std::pair<uint8_t*, size_t>> raw;
    if (uint8_t* m = Manager()) {
        auto* begin = Read<mgr::Region*>(m, mgr::kRegionsBegin);
        auto* end = Read<mgr::Region*>(m, mgr::kRegionsEnd);
        const size_t count = begin && end > begin ? static_cast<size_t>(end - begin) : 0;
        if (count <= kMaxRegions && IsReadable(begin, count * sizeof(mgr::Region))) {
            for (size_t i = 0; i < count; i++) raw.emplace_back(begin[i].ptr, static_cast<size_t>(begin[i].size));
        }
    }
    const bool extras = includeSyncCounters && !raw.empty();
    if (extras && inputUnit_) raw.emplace_back(inputUnit_ + game::inputunit::kCountersBegin, game::inputunit::kCountersSize);
    if (extras && battleFrame_) raw.emplace_back(battleFrame_ + game::battleframe::kStateBegin, game::battleframe::kStateSize);
    uint8_t* singletonTicks = extras ? SingletonTicks() : nullptr;
    if (singletonTicks) raw.emplace_back(singletonTicks, game::singleton::kTicksSize);
    std::vector<const ExtraRegion*> presentation;
    for (const auto& e : presentation_) {
        if (extras && IsReadable(e.ptr, e.size)) presentation.push_back(&e);
    }
    for (const auto* e : presentation) raw.emplace_back(e->ptr, e->size);
    // The round's flow phase and a copy of the round director (flow.h). Not
    // the director itself: it is only known once its task runs, and a region
    // appearing mid-session would change the state size under the session.
    std::vector<ExtraRegion> flow;
    if (extras) {
        flow.push_back({"flow.round", RoundFlowState(), kRoundFlowStateSize});
    }
    for (const auto& e : flow) raw.emplace_back(e.ptr, e.size);
    if (raw == rawLayout_) return false;
    rawLayout_ = raw;
    layoutVersion_++;

    // The registration order differs between battles (and so between peers),
    // so regions get a canonical order: by size, then by contents with
    // pointer-like words ignored. Names follow that order.
    struct Item {
        StateRegion region;
        uint64_t key;
    };
    std::vector<Item> items;
    const size_t gameCount = raw.size() - (extras && inputUnit_ ? 1 : 0) - (extras && battleFrame_ ? 1 : 0) -
                             (singletonTicks ? 1 : 0) - presentation.size() - flow.size();
    for (size_t i = 0; i < gameCount; i++) {
        uint8_t* p = raw[i].first;
        const size_t size = raw[i].second;
        std::vector<uint8_t> copy(p, p + size);
        for (size_t w = 0; w + 8 <= size; w += 8) {
            uint64_t v;
            std::memcpy(&v, &copy[w], 8);
            if (LooksLikeAddress(v)) std::memset(&copy[w], 0, 8);
        }
        items.push_back({{"", p, size, {}}, Hash64(copy.data(), copy.size())});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.region.size != b.region.size ? a.region.size < b.region.size : a.key < b.key;
    });

    std::vector<StateRegion> fresh;
    size_t lastSize = 0;
    int ordinal = 0;
    for (auto& it : items) {
        ordinal = it.region.size == lastSize ? ordinal + 1 : 0;
        lastSize = it.region.size;
        char name[32];
        std::snprintf(name, sizeof(name), "s%zu.%d", it.region.size, ordinal);
        it.region.name = name;
        fresh.push_back(std::move(it.region));
    }
    if (extras && inputUnit_) {
        fresh.push_back({"inputunit.counters", inputUnit_ + game::inputunit::kCountersBegin, game::inputunit::kCountersSize, {}});
    }
    if (extras && battleFrame_) {
        fresh.push_back({"battleframe.state", battleFrame_ + game::battleframe::kStateBegin, game::battleframe::kStateSize, {}});
    }
    if (singletonTicks) fresh.push_back({"singleton.ticks", singletonTicks, game::singleton::kTicksSize, {}});
    for (const auto* e : presentation) fresh.push_back({e->name, e->ptr, e->size, {}});
    for (const auto& e : flow) fresh.push_back({e.name, e.ptr, e.size, {}});

    regions_ = std::move(fresh);
    ArmPendingRegionWatch(regions_, image_.base);
    SetStateMap(stateMap_);
    return true;
}

void SfvGame::SetStateMap(const StateMap* map)
{
    stateMap_ = map;
    const bool fits = map && map->regions.size() == regions_.size() &&
                      std::equal(regions_.begin(), regions_.end(), map->regions.begin(),
                                 [](const StateRegion& r, const StateMap::Region& m) { return r.size == m.size; });
    if (map && !regions_.empty() && !fits) {
        Log("state map does not fit this battle's %zu regions; hashes include pointers (expect false desyncs)",
            regions_.size());
    }
    for (size_t i = 0; i < regions_.size(); i++) {
        StateRegion& r = regions_[i];
        r.ignore = fits ? map->regions[i].ignore : std::vector<uint8_t>{};
        if (r.name == "inputunit.counters") {
            // +0x217c tags the InputUnit's input ring with a reset counter
            // that differs between machines; the logic only compares it with
            // itself. The word's other half (+0x2178) is forced (Advance).
            r.ignore.resize(1, 0);
            r.ignore[0] |= uint8_t(1u << ((game::inputunit::kRingTag - game::inputunit::kCountersBegin) / 8));
        }
        auto it = noise_.find(r.size);
        if (it == noise_.end()) continue;
        r.ignore.resize((r.size / 8 + 8) / 8, 0);
        for (size_t offset : it->second) {
            const size_t word = offset / 8;
            if (word / 8 < r.ignore.size()) r.ignore[word / 8] |= uint8_t(1u << (word % 8));
        }
    }
}

bool SfvGame::LoadNoise(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    noise_.clear();
    unsigned long long size = 0, offset = 0;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (std::sscanf(line, "%llu %llx", &size, &offset) == 2) noise_[size_t(size)].insert(size_t(offset) & ~size_t(7));
    }
    std::fclose(f);
    SetStateMap(stateMap_);
    return true;
}

bool SfvGame::SaveNoise(const std::wstring& path) const
{
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return false;
    std::fprintf(f, "# Words that differ when a frame is resimulated (values derived from engine-side\n"
                    "# transforms). Left out of desync checksums.\n"
                    "# <region size> <word offset, hex>\n");
    for (const auto& [size, offsets] : noise_) {
        for (size_t o : offsets) std::fprintf(f, "%zu 0x%zx\n", size, o);
    }
    return std::fclose(f) == 0;
}

bool SfvGame::AddNoise(size_t regionSize, size_t offset)
{
    if (!noise_[regionSize].insert(offset & ~size_t(7)).second) return false;
    SetStateMap(stateMap_);
    return true;
}

void SfvGame::LearnPointers(StateMap& map) const
{
    if (map.regions.size() != regions_.size()) {
        map.regions.assign(regions_.size(), {});
        for (size_t i = 0; i < regions_.size(); i++) map.regions[i].size = regions_[i].size;
    }
    const auto ranges = CommittedRanges();
    for (size_t i = 0; i < regions_.size(); i++) {
        const StateRegion& r = regions_[i];
        auto& ignore = map.regions[i].ignore;
        ignore.resize((r.size / 8 + 7) / 8, 0);
        for (size_t w = 0; w + 1 <= r.size / 8; w++) {
            uint64_t v;
            std::memcpy(&v, r.ptr + w * 8, 8);
            if (v >= 0x10000 && v < 0x800000000000ull && InRanges(ranges, v)) ignore[w / 8] |= uint8_t(1u << (w % 8));
        }
    }
}

SfvGame::Inventory SfvGame::Inspect() const
{
    Inventory inv;
    uint8_t* m = Manager();
    if (!m) return inv;
    inv.regionCount = regions_.size();
    inv.stateBytes = StateSize(regions_);
    auto* groups = Read<mgr::Group*>(m, mgr::kGroupsBegin);
    auto* groupsEnd = Read<mgr::Group*>(m, mgr::kGroupsEnd);
    inv.groupCount = groups && groupsEnd > groups ? static_cast<size_t>(groupsEnd - groups) : 0;
    inv.ringCount = Read<uint64_t>(m, mgr::kRingCount);
    inv.ringMapSize = Read<uint64_t>(m, mgr::kRingMapSize);
    inv.currentId = Read<game::FrameId>(m, mgr::kCurrentId);

    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (IsReadable(sentinel, 8)) {
        for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext);
             node && node != sentinel && inv.postRestoreCallbacks < 1000 && IsReadable(node, 0x38);
             node = Read<uint8_t*>(node, mgr::callback::kNext)) {
            inv.postRestoreCallbacks++;
        }
    }
    return inv;
}

void SfvGame::LogLayout() const
{
    uint8_t* m = Manager();
    if (!m) {
        Log("layout: no rollback manager");
        return;
    }
    const Inventory inv = Inspect();
    Log("layout: manager %p, %zu regions, %llu bytes, %zu groups, ring %llu frames (map %llu), current id %d:%d, "
        "%zu post-restore callbacks",
        m, inv.regionCount, static_cast<unsigned long long>(inv.stateBytes), inv.groupCount,
        static_cast<unsigned long long>(inv.ringCount), static_cast<unsigned long long>(inv.ringMapSize),
        inv.currentId.lo, inv.currentId.hi, inv.postRestoreCallbacks);

    auto* groups = Read<mgr::Group*>(m, mgr::kGroupsBegin);
    for (size_t g = 0; g < inv.groupCount; g++) {
        Log("  group %zu: regions %llu..%llu -> %p", g, static_cast<unsigned long long>(groups[g].first),
            static_cast<unsigned long long>(groups[g].last), groups[g].dst);
    }

    const auto ranges = CommittedRanges();
    for (size_t i = 0; i < regions_.size(); i++) {
        const StateRegion& r = regions_[i];
        size_t pointers = 0;
        for (size_t w = 0; w + 1 <= r.size / 8; w++) {
            uint64_t v;
            std::memcpy(&v, r.ptr + w * 8, 8);
            if (v >= 0x10000 && v < 0x800000000000ull && InRanges(ranges, v)) pointers++;
        }
        const uintptr_t p = reinterpret_cast<uintptr_t>(r.ptr);
        const bool inImage = p >= image_.base && p < image_.base + 0x5000000;
        Log("  region %-10s %p %8zu bytes  %s  %zu pointer-like words%s", r.name.c_str(), r.ptr, r.size,
            inImage ? "image" : "heap ", pointers, (p & 7) ? "  (unaligned)" : "");
    }

    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (IsReadable(sentinel, 8)) {
        int n = 0;
        for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext);
             node && node != sentinel && n < 64 && IsReadable(node, 0x38);
             node = Read<uint8_t*>(node, mgr::callback::kNext), n++) {
            const auto fn = Read<uintptr_t>(node, mgr::callback::kFunction);
            // Both thunks seen in practice call ctx->target(object): log the target.
            const auto* ctx = Read<const uint8_t*>(node, mgr::callback::kContext);
            const uintptr_t target = IsReadable(ctx, 8) ? Read<uintptr_t>(ctx, 0) : 0;
            Log("  post-restore callback %d: thunk rva 0x%llX target rva 0x%llX", n,
                static_cast<unsigned long long>(fn - image_.base),
                static_cast<unsigned long long>(target ? target - image_.base : 0));
        }
    }
}

bool SfvGame::RestoreRingFrame(const game::FrameId& id, bool discardNewer)
{
    uint8_t* m = Manager();
    if (!m) return false;
    using RestoreFn = int (*)(void*, const game::FrameId*, int);
    return image_.At<RestoreFn>(game::kManagerRestore.rva)(m, &id, discardNewer ? 1 : 0) != 0;
}

void SfvGame::RunPostRestoreCallbacks(const game::FrameId& id) const
{
    // Mirrors the loop at 0x48A45C in Restore.
    uint8_t* m = Manager();
    if (!m) return;
    const uint8_t* sentinel = Read<uint8_t*>(m, mgr::kPostRestoreList);
    if (!IsReadable(sentinel, 8)) return;
    using CallbackFn = void (*)(void* ctx, const game::FrameId*);
    for (const uint8_t* node = Read<uint8_t*>(sentinel, mgr::callback::kNext); node && node != sentinel;
         node = Read<uint8_t*>(node, mgr::callback::kNext)) {
        auto fn = Read<CallbackFn>(node, mgr::callback::kFunction);
        if (fn && Read<uint64_t>(node, mgr::callback::kToken) != 0) fn(Read<void*>(node, mgr::callback::kContext), &id);
    }
}

std::string SfvGame::BuildId() const
{
    char buf[96];
    uint64_t noise = 0;
    for (const auto& [size, offsets] : noise_) {
        for (size_t o : offsets) {
            const uint64_t pair[2] = {size, o};
            noise = Hash64(pair, sizeof(pair), noise);
        }
    }
    std::snprintf(buf, sizeof(buf), "sfv-%08X/adapter-3/map-%016llx/noise-%016llx", image_.timestamp,
                  static_cast<unsigned long long>(stateMap_ ? stateMap_->Fingerprint() : 0),
                  static_cast<unsigned long long>(noise));
    return buf;
}

bool SfvGame::BeginMatch(const MatchSetup& setup)
{
    // The battle itself is set up and entered beforehand (battle_setup.h); a
    // session starts once that battle runs and its objects have been seen.
    (void)setup;
    RefreshRegions();
    return CanRunTicks() && !regions_.empty();
}

PackedInput SfvGame::ReadLocalInput(int player)
{
    if (inputOverride) return inputOverride(player);
    // What the game computed from the physical pad on the last tick. Netplay
    // plays the local player with the input manager's pad 0 (keyboard and
    // pads, with the player's button config) on both machines.
    if (localInput && !bothPadsLocal) return localInput(localPadSlot);
    return sampled_[bothPadsLocal ? std::clamp(player, 0, 3) : localPadSlot];
}

void SfvGame::Advance(const FrameInputs& inputs, bool rollingBack)
{
    if (!CanRunTicks()) return;
    hitch::AddTick(rollingBack); // the frame-drop log: ticks per frame, and how many replayed
    // Start reaches the game only when both players pressed it outside the
    // fight (flow.h): one player cannot skip the intro, or pause the fight.
    const PackedInput start = PresentationStart(inputs.p[0], inputs.p[1]);
    inject_[0] = (inputs.p[0] & ~kStart) | start;
    inject_[1] = (inputs.p[1] & ~kStart) | start;
    // Set at round start from a machine-specific global (rva 0x438EBD): it
    // differs between peers (0 on one, 1 on the other) and the battle
    // diverges when the fight starts. Same value everywhere.
    if (inputUnit_) *reinterpret_cast<int32_t*>(inputUnit_ + game::inputunit::kRoundParam) = 0;
    injecting_ = true;
    BeginDrivenTick();
    // One tick: BattleFrame::Update runs 1 + [this+0x1A8] ticks.
    *reinterpret_cast<int32_t*>(battleFrame_ + game::battleframe::kExtraTicks) = 0;
    runFrame_(battleFrame_);
    injecting_ = false;
    // The round director's check for this tick, and its state into the snapshot.
    RunRoundDirector();
    CheckChangeWatch(regions_, image_.base);
    // Between ticks the presentation (intro and round call skipping, once
    // per rendered frame) reads the current input values. Rendered frames and
    // ticks line up differently on each machine, so it must see nothing: a
    // skip pressed during an intro would otherwise start the fight on a
    // different tick per machine.
    if (inputUnit_ && exclusiveTicks) {
        auto* values = reinterpret_cast<uint32_t*>(inputUnit_ + game::inputunit::kValues);
        for (int i = 0; i < 4; i++) values[i] = 0;
    }
}

bool SfvGame::OutcomeDecided() { return MatchDecided(); }

bool SfvGame::MatchWins(uint32_t& left, uint32_t& right) const
{
    // The battle's round state is the one 228-byte region: +0xb4 the round,
    // +0xc4 / +0xc8 the matches each side won since character select (one
    // more as the match is decided; found from recordings, 2026-10-28).
    constexpr size_t kRoundStateSize = 228;
    for (const StateRegion& r : regions_) {
        if (r.size != kRoundStateSize || !IsReadable(r.ptr, r.size)) continue;
        std::memcpy(&left, r.ptr + 0xC4, 4);
        std::memcpy(&right, r.ptr + 0xC8, 4);
        return true;
    }
    return false;
}

void SfvGame::OnStateLoaded(int frame)
{
    // The callbacks receive the id of the restored frame. Ours are not ids the
    // game issued, so a callback that looks up its own per-frame data by id
    // would fail; the probe log lists the callbacks to check that statically.
    RunPostRestoreCallbacks(game::FrameId{frame, 0x5346});
    RoundDirectorFromSnapshot();
    MatchStopToDirector(battleFrame_);
}

} // namespace sfv::mod
