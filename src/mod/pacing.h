#pragma once
// Frame pacing during netplay matches. Time sync used to drop whole frames on
// the peer that runs ahead (a visible repeat, about ten a minute). Now that
// peer stretches its frame period a little (StretchFor) instead: right before
// the game's per-frame input poll (control.cpp), so no input waits for it,
// the game thread waits until the last frame's time here plus the stretched
// period (QPC: a high-resolution timer, then a spin). Without a stretch there
// is no wait and the engine's own limiter paces as before. A hitch is not
// caught up: after one the next frame starts the measure afresh.

#include <cstdint>

namespace sfv::mod {

// The period stretch for a lead of `ahead` frames (GekkoNet's averaged
// estimate): none up to 0.25, then 1 % per frame up to 3 frames (a one-frame
// lead is gone in about 1.7 s, slower than the estimate's ~0.4 s lag, so it
// does not overshoot), then 3 % plus 3 % per frame beyond 3, at most 25 %.
float StretchFor(float ahead);

// From the netplay driver's frame while a match runs: this side's lead. Pacing
// is on while these keep coming (it lapses a quarter of a second after the last).
void PaceLead(float ahead);

// From the game thread's input update, once per frame: waits for the deadline.
void PaceFrame();

struct PaceStats {
    int paced = 0;     // frames the pacer handled
    int late = 0;      // stretched frames that reached it past their deadline already (no wait)
    int resets = 0;    // frames after a hitch (over 100 ms since the last), not paced
    int stretched = 0; // frames with a stretch
    double stretchSum = 0; // their stretch, summed (fractions)
    double waitedMs = 0;   // time spent waiting
};
// The figures since the last call (the netplay pace log).
PaceStats TakePaceStats();

} // namespace sfv::mod
