#pragma once
// Finding frame drops. The parts of a frame time themselves as they run, the
// game thread's (the netplay frame with its rollback ticks, the game's own
// battle frame, the transport and its lock) and the render thread's (the
// overlay, the Present call), adding up since the last presented frame.
// When a frame is presented the render thread checks how long it took: a
// frame over kSlowMs counts as slow; one over kHitchMs is logged at once with
// where its time went, the rest are summed into one line a second (the
// count and the slowest frame's breakdown). Time no part claims is the
// engine's own (its game thread outside the battle, streaming, garbage
// collection) or the system's.

#include <chrono>
#include <cstdint>

namespace sfv::mod::hitch {

constexpr double kSlowMs = 25;   // more than 1.5 frames at 60 fps
constexpr double kHitchMs = 100; // logged on its own

enum class Part {
    NetFrame,      // the netplay driver's frame: its ticks, rollbacks and session work
    BattleFrame,   // the game's battle frame outside the driver
    Transport,     // sends and receives on the Weyvelength client
    TransportWait, // ... of which waiting for the client's lock
    Overlay,       // the overlay's ImGui frame (render thread)
    Present,       // the swap chain's Present (render thread: GPU, vsync, driver)
    Count
};

void Add(Part part, int64_t micros);
void AddTick(bool replayed); // a battle tick the driver ran; replayed: a rollback's re-simulation

// The render thread, right after a Present: frameMicros since the last one.
// inMatch: a netplay match runs (outside one, loading screens and menus take
// long frames by design; the parts are reset but nothing is logged).
void FramePresented(int64_t frameMicros, bool inMatch);

// Adds the time from construction to destruction to a part.
class Scope {
public:
    explicit Scope(Part part) : part_(part), start_(std::chrono::steady_clock::now()) {}
    ~Scope()
    {
        Add(part_, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Part part_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace sfv::mod::hitch
