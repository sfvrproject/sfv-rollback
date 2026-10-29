#pragma once
// A small deterministic 2D fighter behind IGame. It exists so the netplay
// driver, the replay recorder and the desync tooling can be exercised end to
// end without Street Fighter V, including with the kinds of bugs that cause
// real rollback desyncs, which can be switched on one at a time.

#include "core/game.h"

#include <functional>

namespace sfv {

enum class MockBug {
    None,
    // Sim state that is not part of the snapshot: the camera, which clamps how
    // far apart the fighters can walk, is left out of the region list. Only
    // shows after a rollback.
    StateOutsideSnapshot,
    // A per-machine value (think: a local option or uninitialised memory)
    // feeds into damage. Diverges without any rollback.
    MachineSpecificValue,
    // A hit spark effect is skipped while rolling back, as it should be, but it
    // draws from the gameplay RNG, so resimulated frames consume less RNG.
    EffectUsesGameplayRng,
};

struct MockGameOptions {
    MockBug bug = MockBug::None;
    uint32_t machineValue = 0; // used by MachineSpecificValue
    // Local controller: returns the input for the n-th poll of `player`.
    std::function<PackedInput(int poll, int player)> input;
};

class MockGame : public IGame {
public:
    explicit MockGame(MockGameOptions options = {});

    std::string BuildId() const override { return "mockfighter-1"; }
    bool BeginMatch(const MatchSetup& setup) override;
    const std::vector<StateRegion>& Regions() override { return regions_; }
    PackedInput ReadLocalInput(int player) override;
    void Advance(const FrameInputs& inputs, bool rollingBack) override;

    int Frame() const { return round_.frame; }
    int Health(int player) const { return fighters_[player].hp; }
    int Wins(int player) const { return round_.wins[player]; }

private:
    struct Fighter {
        int32_t x, y, vx, vy;
        int32_t hp;
        int32_t meter;
        int16_t state;
        int16_t timer;
        int16_t attack;
        int16_t facing;
        int16_t stun;
        int16_t combo;
        uint8_t attackHit;
        uint8_t pad[3];
    };
    struct Projectile {
        int32_t x, vx;
        int16_t owner;
        int16_t life;
    };
    struct Round {
        int32_t frame;
        int32_t timer;
        int32_t pause;
        int16_t wins[kNumPlayers];
        int16_t roundsToWin;
        int16_t matchOver;
    };
    struct Camera {
        int32_t x;
    };

    uint32_t NextRng();
    void ResetRound();
    void StepFighter(int p, PackedInput in);
    void ResolveAttack(int p, bool rollingBack);
    void StepProjectiles(bool rollingBack);
    void Hit(int victim, int damage, int stun, int push, bool rollingBack);
    void UpdateCamera();
    void EndRoundIfOver();

    MockGameOptions options_;
    Fighter fighters_[kNumPlayers] = {};
    Projectile projectiles_[8] = {};
    uint32_t rng_ = 1;
    Round round_ = {};
    Camera camera_ = {};
    uint32_t sparks_ = 0; // presentation only
    int polls_[kNumPlayers] = {};
    std::vector<StateRegion> regions_;
};

} // namespace sfv
