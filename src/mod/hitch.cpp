#include "mod/hitch.h"

#include "mod/log.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <windows.h>

namespace sfv::mod::hitch {

namespace {

constexpr int kParts = static_cast<int>(Part::Count);
std::atomic<int64_t> g_parts[kParts]; // microseconds since the last presented frame
std::atomic<int> g_ticks{0};
std::atomic<int> g_replayed{0};

struct Frame {
    int64_t micros = 0;
    int64_t parts[kParts] = {};
    int ticks = 0;
    int replayed = 0;
};

// The render thread's: the current second's slow frames.
ULONGLONG g_windowStart = 0;
int g_slow = 0;
int64_t g_slowExtraMicros = 0; // time over 1/60 s, summed
Frame g_slowest;

std::string Describe(const Frame& f)
{
    auto ms = [&](Part p) { return f.parts[static_cast<int>(p)] / 1000.0; };
    const double game = ms(Part::NetFrame) + ms(Part::BattleFrame) + ms(Part::Pacing);
    const double render = ms(Part::Overlay) + ms(Part::Present);
    // Game and render threads overlap, so "elsewhere" is what the longer of
    // the two leaves unaccounted.
    const double elsewhere = f.micros / 1000.0 - (game > render ? game : render);
    char buf[340];
    std::snprintf(buf, sizeof(buf),
                  "%.0f ms: netplay %.1f ms (%d ticks, %d replayed), battle %.1f ms, pacing %.1f ms, transport %.1f ms "
                  "(lock wait %.1f), overlay %.1f ms, present %.1f ms, elsewhere %.0f ms",
                  f.micros / 1000.0, ms(Part::NetFrame), f.ticks, f.replayed, ms(Part::BattleFrame), ms(Part::Pacing),
                  ms(Part::Transport), ms(Part::TransportWait), ms(Part::Overlay), ms(Part::Present),
                  elsewhere > 0 ? elsewhere : 0.0);
    return buf;
}

} // namespace

void Add(Part part, int64_t micros)
{
    g_parts[static_cast<int>(part)].fetch_add(micros, std::memory_order_relaxed);
}

void AddTick(bool replayed)
{
    g_ticks.fetch_add(1, std::memory_order_relaxed);
    if (replayed) g_replayed.fetch_add(1, std::memory_order_relaxed);
}

void FramePresented(int64_t frameMicros, bool inMatch)
{
    Frame f;
    f.micros = frameMicros;
    for (int i = 0; i < kParts; i++) f.parts[i] = g_parts[i].exchange(0, std::memory_order_relaxed);
    f.ticks = g_ticks.exchange(0, std::memory_order_relaxed);
    f.replayed = g_replayed.exchange(0, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (!inMatch || frameMicros > 2'000'000) return; // a pause (loading, a breakpoint), not a drop
    const double ms = frameMicros / 1000.0;
    if (ms >= kHitchMs) Log("hitch: one frame took %s", Describe(f).c_str());
    if (ms >= kSlowMs) {
        g_slow++;
        g_slowExtraMicros += frameMicros - 16667;
        if (f.micros > g_slowest.micros) g_slowest = f;
    }
    if (now - g_windowStart >= 1000) {
        if (g_slow > 0) {
            Log("hitch: %d slow frames in the last second (%.0f ms lost), the slowest %s", g_slow, g_slowExtraMicros / 1000.0,
                Describe(g_slowest).c_str());
        }
        g_windowStart = now;
        g_slow = 0;
        g_slowExtraMicros = 0;
        g_slowest = Frame{};
    }
}

} // namespace sfv::mod::hitch
