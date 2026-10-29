#include "mod/outside_probe.h"

#include "core/state.h"
#include "mod/battle_driver.h"
#include "mod/log.h"
#include "mod/sfv_game.h"
#include "mod/ue4.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace sfv::mod {

namespace {

struct Copy {
    const void* object;
    size_t size;
    size_t at; // in the buffer
};

void Capture(const std::vector<Copy>& objects, std::vector<uint8_t>& buffer)
{
    for (const Copy& c : objects) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), c.object, &buffer[c.at], c.size, &got) || got != c.size) {
            std::memset(&buffer[c.at], 0, c.size);
        }
    }
}

// Committed private read-write pages of the process (4 KB each).
std::vector<uintptr_t> WritablePages()
{
    std::vector<uintptr_t> pages;
    ULONG_PTR stackLow = 0, stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t p = base; p < end; p += 4096) {
                if (p >= stackLow && p < stackHigh) continue; // this thread's stack
                pages.push_back(p);
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    return pages;
}

void HashPages(const std::vector<uintptr_t>& pages, std::vector<uint64_t>& out)
{
    out.resize(pages.size());
    std::vector<uint8_t> buf(4096);
    for (size_t i = 0; i < pages.size(); i++) {
        SIZE_T got = 0;
        out[i] = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(pages[i]), buf.data(), 4096, &got)
                     ? Hash64(buf.data(), 4096)
                     : 0;
    }
}

void CopyPages(const std::vector<uintptr_t>& pages, std::vector<uint8_t>& out)
{
    out.assign(pages.size() * 4096, 0);
    for (size_t i = 0; i < pages.size(); i++) {
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(pages[i]), &out[i * 4096], 4096, &got);
    }
}

struct ObjectRange {
    uintptr_t lo, hi;
    const void* object;
};

// The UObject (from the object array) whose memory holds `address`, or null.
const void* OwnerOf(const std::vector<ObjectRange>& objects, uintptr_t address)
{
    auto it = std::upper_bound(objects.begin(), objects.end(), address,
                               [](uintptr_t a, const ObjectRange& r) { return a < r.lo; });
    if (it == objects.begin()) return nullptr;
    --it;
    return address < it->hi ? it->object : nullptr;
}

// Same idea over all writable memory: pages the ticks changed and the
// snapshot did not restore (hashes find the pages, a second run the bytes).
class PageProbe : public IFrameDriver {
public:
    explicit PageProbe(int ticks) : ticks_(ticks) {}

    bool OnFrame(SfvGame& game) override
    {
        SetFrameDriver(nullptr);
        Run(game);
        delete this;
        return true;
    }

private:
    void TicksAndBack(SfvGame& game, const std::vector<StateRegion>& regions, std::vector<uint8_t>& state)
    {
        for (int i = 0; i < ticks_; i++) game.Advance(FrameInputs{}, false);
        LoadRegions(regions, state.data());
        game.OnStateLoaded(0);
    }

    void Run(SfvGame& game)
    {
        game.RefreshRegions();
        const auto& regions = game.Regions();
        if (regions.empty() || !game.CanRunTicks()) {
            Log("outside pages: no battle state to test");
            return;
        }
        std::vector<uint8_t> state(StateSize(regions));
        const std::vector<uintptr_t> pages = WritablePages();

        std::vector<uint64_t> a, b, c;
        SaveRegions(regions, state.data());
        HashPages(pages, a);
        HashPages(pages, b);
        TicksAndBack(game, regions, state);
        HashPages(pages, c);
        std::vector<uintptr_t> candidates;
        for (size_t i = 0; i < pages.size(); i++) {
            if (a[i] == b[i] && a[i] != c[i]) candidates.push_back(pages[i]);
        }
        Log("outside pages: %zu pages (%zu MB), %zu changed by %d ticks and not restored", pages.size(),
            pages.size() >> 8, candidates.size(), ticks_);

        std::vector<uint8_t> before, still, after;
        SaveRegions(regions, state.data());
        CopyPages(candidates, before);
        CopyPages(candidates, still);
        TicksAndBack(game, regions, state);
        CopyPages(candidates, after);

        std::vector<ObjectRange> objects;
        ue4::ForEachObject([&](const void* object) {
            const size_t size = ue4::ObjectSize(object);
            const auto lo = reinterpret_cast<uintptr_t>(object);
            if (size) objects.push_back({lo, lo + size, object});
        });
        std::sort(objects.begin(), objects.end(), [](const ObjectRange& x, const ObjectRange& y) { return x.lo < y.lo; });

        int shown = 0;
        for (size_t p = 0; p < candidates.size() && shown < 400; p++) {
            for (size_t w = 0; w < 4096 && shown < 400; w += 8) {
                const size_t at = p * 4096 + w;
                if (std::memcmp(&before[at], &still[at], 8) != 0 || std::memcmp(&before[at], &after[at], 8) == 0) continue;
                uint64_t x, y;
                std::memcpy(&x, &before[at], 8);
                std::memcpy(&y, &after[at], 8);
                const uintptr_t address = candidates[p] + w;
                const void* owner = OwnerOf(objects, address);
                char where[64] = "(not in a UObject)";
                if (owner) std::snprintf(where, sizeof(where), "+0x%llx", static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(owner)));
                Log("  %p: %016llx -> %016llx  %s %s", reinterpret_cast<void*>(address), static_cast<unsigned long long>(x),
                    static_cast<unsigned long long>(y), owner ? ue4::Describe(owner).c_str() : "", where);
                shown++;
            }
        }
    }

    int ticks_;
};

class OutsideProbe : public IFrameDriver {
public:
    explicit OutsideProbe(int ticks) : ticks_(ticks) {}

    bool OnFrame(SfvGame& game) override
    {
        SetFrameDriver(nullptr);
        Run(game);
        delete this;
        return true;
    }

private:
    void Run(SfvGame& game)
    {
        game.RefreshRegions();
        const auto& regions = game.Regions();
        if (regions.empty() || !game.CanRunTicks()) {
            Log("outside: no battle state to test");
            return;
        }
        std::vector<std::pair<uintptr_t, uintptr_t>> inSnapshot;
        for (const auto& r : regions) {
            inSnapshot.emplace_back(reinterpret_cast<uintptr_t>(r.ptr), reinterpret_cast<uintptr_t>(r.ptr) + r.size);
        }
        auto covered = [&](uintptr_t a) {
            for (const auto& [lo, hi] : inSnapshot) {
                if (a >= lo && a < hi) return true;
            }
            return false;
        };

        std::vector<Copy> objects;
        size_t total = 0;
        ue4::ForEachObject([&](const void* object) {
            const size_t size = ue4::ObjectSize(object) & ~size_t(7);
            if (!size) return;
            objects.push_back({object, size, total});
            total += size;
        });
        std::vector<uint8_t> before(total), still(total), after(total);
        std::vector<uint8_t> state(StateSize(regions));
        SaveRegions(regions, state.data());
        Capture(objects, before);
        Capture(objects, still); // no ticks: what changes anyway
        for (int i = 0; i < ticks_; i++) game.Advance(FrameInputs{}, false);
        LoadRegions(regions, state.data());
        game.OnStateLoaded(0);
        Capture(objects, after);

        struct Hit {
            size_t index;
            int words;
        };
        std::vector<Hit> hits;
        for (size_t i = 0; i < objects.size(); i++) {
            const Copy& c = objects[i];
            int words = 0;
            for (size_t w = 0; w < c.size; w += 8) {
                const uint8_t* b = &before[c.at + w];
                if (std::memcmp(b, &still[c.at + w], 8) != 0) continue; // changes without ticks
                if (std::memcmp(b, &after[c.at + w], 8) == 0) continue;
                if (covered(reinterpret_cast<uintptr_t>(c.object) + w)) continue;
                words++;
            }
            if (words) hits.push_back({i, words});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.words > b.words; });
        Log("outside: %zu objects (%zu MB) checked, %zu changed by %d ticks and not restored", objects.size(),
            total >> 20, hits.size(), ticks_);
        for (size_t h = 0; h < hits.size() && h < 80; h++) {
            const Copy& c = objects[hits[h].index];
            std::string detail;
            int shown = 0;
            // All words for the game's own blueprint objects (KAB_ animation,
            // KBP_ battle); a few for the rest.
            const std::string cls = ue4::ObjectName(ue4::ClassOf(c.object));
            const int limit = cls.rfind("KAB_", 0) == 0 || cls.rfind("KBP_", 0) == 0 ? 40 : 4;
            for (size_t w = 0; w < c.size && shown < limit; w += 8) {
                const uint8_t* b = &before[c.at + w];
                if (std::memcmp(b, &still[c.at + w], 8) != 0 || std::memcmp(b, &after[c.at + w], 8) == 0) continue;
                uint64_t x, y;
                std::memcpy(&x, b, 8);
                std::memcpy(&y, &after[c.at + w], 8);
                char buf[80];
                std::snprintf(buf, sizeof(buf), " +0x%zx %llx->%llx", w, static_cast<unsigned long long>(x),
                              static_cast<unsigned long long>(y));
                detail += buf;
                shown++;
            }
            Log("  %3d words  %s (%p) in %s, size 0x%zx:%s", hits[h].words, ue4::Describe(c.object).c_str(), c.object,
                ue4::ObjectName(ue4::OuterOf(c.object)).c_str(), c.size, detail.c_str());
        }
    }

    int ticks_;
};

} // namespace

void RequestOutsideProbe(SfvGame* game, int ticks, bool allMemory)
{
    (void)game;
    if (CurrentFrameDriver()) {
        Log("outside: a frame driver (netplay?) is running; leave first");
        return;
    }
    // The probe deletes itself after its one frame.
    if (allMemory) SetFrameDriver(new PageProbe(ticks));
    else SetFrameDriver(new OutsideProbe(ticks));
}

} // namespace sfv::mod
