#include "testbed/mock_game.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace sfv {

namespace {

enum State : int16_t { kIdle, kWalk, kJump, kCrouch, kAttack, kHitstun, kBlockstun, kKO };

struct AttackData {
    int16_t startup, active, recovery;
    int32_t range, damage;
    int16_t stun;
    int32_t push;
};

// LP MP HP LK MK HK
constexpr AttackData kAttacks[6] = {
    {3, 2, 6, 70, 30, 12, 6},   {5, 3, 10, 90, 60, 16, 8},  {8, 3, 18, 110, 90, 20, 10},
    {4, 2, 8, 80, 35, 12, 6},   {6, 3, 12, 100, 65, 16, 9}, {10, 4, 20, 130, 100, 22, 12},
};

constexpr int32_t kStageHalfWidth = 1000;
constexpr int32_t kScreenHalfWidth = 220;
constexpr int32_t kRoundFrames = 99 * 60;

} // namespace

MockGame::MockGame(MockGameOptions options) : options_(std::move(options))
{
    regions_.push_back({"fighters", reinterpret_cast<uint8_t*>(fighters_), sizeof(fighters_)});
    regions_.push_back({"projectiles", reinterpret_cast<uint8_t*>(projectiles_), sizeof(projectiles_)});
    regions_.push_back({"rng", reinterpret_cast<uint8_t*>(&rng_), sizeof(rng_)});
    regions_.push_back({"round", reinterpret_cast<uint8_t*>(&round_), sizeof(round_)});
    if (options_.bug != MockBug::StateOutsideSnapshot) {
        regions_.push_back({"camera", reinterpret_cast<uint8_t*>(&camera_), sizeof(camera_)});
    }
}

bool MockGame::BeginMatch(const MatchSetup& setup)
{
    std::memset(fighters_, 0, sizeof(fighters_));
    std::memset(projectiles_, 0, sizeof(projectiles_));
    std::memset(&round_, 0, sizeof(round_));
    camera_ = {};
    rng_ = static_cast<uint32_t>(setup.seed ^ (setup.seed >> 32)) | 1u;
    round_.roundsToWin = setup.roundsToWin ? setup.roundsToWin : 2;
    // Character choice changes health, so a setup mismatch shows in the state.
    for (int p = 0; p < kNumPlayers; p++) fighters_[p].meter = setup.players[p].character;
    ResetRound();
    for (int p = 0; p < kNumPlayers; p++) fighters_[p].hp = 1000 + setup.players[p].character * 10;
    return true;
}

PackedInput MockGame::ReadLocalInput(int player)
{
    const int poll = polls_[player]++;
    return options_.input ? options_.input(poll, player) : 0;
}

uint32_t MockGame::NextRng()
{
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

void MockGame::ResetRound()
{
    for (int p = 0; p < kNumPlayers; p++) {
        Fighter& f = fighters_[p];
        const int32_t meter = f.meter;
        std::memset(&f, 0, sizeof(f));
        f.x = p == 0 ? -150 : 150;
        f.hp = 1000;
        f.meter = meter;
        f.facing = p == 0 ? 1 : -1;
    }
    std::memset(projectiles_, 0, sizeof(projectiles_));
    round_.timer = kRoundFrames;
    round_.pause = 0;
    camera_.x = 0;
}

void MockGame::Advance(const FrameInputs& inputs, bool rollingBack)
{
    round_.frame++;

    if (round_.matchOver) return;
    if (round_.pause > 0) {
        if (--round_.pause == 0) ResetRound();
        return;
    }

    for (int p = 0; p < kNumPlayers; p++) StepFighter(p, inputs.p[p]);
    for (int p = 0; p < kNumPlayers; p++) ResolveAttack(p, rollingBack);
    StepProjectiles(rollingBack);
    UpdateCamera();

    for (int p = 0; p < kNumPlayers; p++) {
        Fighter& f = fighters_[p];
        const Fighter& o = fighters_[1 - p];
        if (f.state != kAttack && f.y == 0) f.facing = o.x >= f.x ? 1 : -1;
    }

    round_.timer--;
    EndRoundIfOver();
}

void MockGame::StepFighter(int p, PackedInput in)
{
    Fighter& f = fighters_[p];

    const bool forward = (in & (f.facing > 0 ? kRight : kLeft)) != 0;
    const bool back = (in & (f.facing > 0 ? kLeft : kRight)) != 0;

    switch (f.state) {
    case kHitstun:
    case kBlockstun:
        f.vx = f.vx * 3 / 4;
        if (--f.stun <= 0) {
            f.state = kIdle;
            f.combo = 0;
        }
        break;
    case kAttack: {
        const AttackData& a = kAttacks[f.attack];
        f.vx = 0;
        if (++f.timer >= a.startup + a.active + a.recovery) f.state = kIdle;
        break;
    }
    case kJump:
        break;
    default: {
        f.vx = 0;
        f.state = kIdle;
        int attack = -1;
        for (int b = 0; b < 6; b++) {
            if (in & (kLP << b)) {
                attack = b;
                break;
            }
        }
        if (attack == 5 && (in & kDown) && f.meter >= 50) {
            for (auto& pr : projectiles_) {
                if (pr.life > 0) continue;
                pr = {f.x + f.facing * 40, f.facing * 9, static_cast<int16_t>(p), 120};
                f.meter -= 50;
                f.state = kAttack;
                f.attack = 5;
                f.timer = 0;
                f.attackHit = 1; // the projectile does the damage
                break;
            }
        }
        else if (attack >= 0) {
            f.state = kAttack;
            f.attack = static_cast<int16_t>(attack);
            f.timer = 0;
            f.attackHit = 0;
        }
        else if (in & kUp) {
            f.state = kJump;
            f.vy = 22;
            f.vx = forward ? 5 * f.facing : back ? -5 * f.facing : 0;
        }
        else if (in & kDown) {
            f.state = kCrouch;
        }
        else if (forward || back) {
            f.state = kWalk;
            f.vx = (forward ? 4 : -3) * f.facing;
        }
        break;
    }
    }

    if (f.y > 0 || f.vy > 0) {
        f.y += f.vy;
        f.vy -= 2;
        if (f.y <= 0) {
            f.y = 0;
            f.vy = 0;
            if (f.state == kJump) f.state = kIdle;
        }
    }

    // Walking toward the screen edge is slowed, so the camera matters every frame.
    if (f.state == kWalk && std::abs(f.x + f.vx - camera_.x) > kScreenHalfWidth / 2) f.vx /= 2;
    f.x += f.vx;
    f.x = std::clamp(f.x, -kStageHalfWidth, kStageHalfWidth);
    f.x = std::clamp(f.x, camera_.x - kScreenHalfWidth, camera_.x + kScreenHalfWidth);
    f.meter = std::min(f.meter + 1, 100);
}

void MockGame::ResolveAttack(int p, bool rollingBack)
{
    Fighter& f = fighters_[p];
    if (f.state != kAttack || f.attackHit) return;
    const AttackData& a = kAttacks[f.attack];
    if (f.timer < a.startup || f.timer >= a.startup + a.active) return;

    Fighter& o = fighters_[1 - p];
    if (std::abs(o.x - f.x) > a.range || o.y > 80) return;

    f.attackHit = 1;
    const bool blocking = o.state == kCrouch || o.state == kBlockstun;
    if (blocking) {
        o.state = kBlockstun;
        o.stun = a.stun / 2;
        o.vx = f.facing * a.push;
        o.hp -= a.damage / 8;
        return;
    }
    const int damage = a.damage + static_cast<int>(NextRng() % 8);
    Hit(1 - p, damage, a.stun, f.facing * a.push, rollingBack);
}

void MockGame::StepProjectiles(bool rollingBack)
{
    for (auto& pr : projectiles_) {
        if (pr.life <= 0) continue;
        pr.x += pr.vx;
        pr.life--;
        if (std::abs(pr.x) > kStageHalfWidth) {
            pr.life = 0;
            continue;
        }
        const int victim = 1 - pr.owner;
        const Fighter& o = fighters_[victim];
        if (std::abs(o.x - pr.x) < 30 && o.y < 60) {
            int damage = 70;
            if (options_.bug == MockBug::MachineSpecificValue) damage += static_cast<int>(options_.machineValue % 5);
            Hit(victim, damage, 18, pr.vx > 0 ? 8 : -8, rollingBack);
            pr.life = 0;
        }
    }
}

void MockGame::Hit(int victim, int damage, int stun, int push, bool rollingBack)
{
    Fighter& o = fighters_[victim];
    o.combo++;
    o.hp -= damage * 10 / (9 + o.combo);
    o.state = kHitstun;
    o.stun = static_cast<int16_t>(stun);
    o.vx = push;
    fighters_[1 - victim].meter = std::min(fighters_[1 - victim].meter + 8, 100);

    // Effects are presentation: skipped while resimulating.
    if (!rollingBack) {
        sparks_ = options_.bug == MockBug::EffectUsesGameplayRng ? NextRng() : sparks_ * 1664525u + 1013904223u;
    }
}

void MockGame::UpdateCamera()
{
    const int32_t mid = (fighters_[0].x + fighters_[1].x) / 2;
    camera_.x += (mid - camera_.x) / 6;
    camera_.x = std::clamp(camera_.x, -kStageHalfWidth + kScreenHalfWidth, kStageHalfWidth - kScreenHalfWidth);
}

void MockGame::EndRoundIfOver()
{
    const bool ko0 = fighters_[0].hp <= 0, ko1 = fighters_[1].hp <= 0;
    if (!ko0 && !ko1 && round_.timer > 0) return;

    int winner = -1;
    if (ko0 != ko1) winner = ko0 ? 1 : 0;
    else if (fighters_[0].hp != fighters_[1].hp) winner = fighters_[0].hp > fighters_[1].hp ? 0 : 1;

    if (winner >= 0) round_.wins[winner]++;
    if (winner >= 0 && round_.wins[winner] >= round_.roundsToWin) {
        round_.matchOver = 1;
        return;
    }
    round_.pause = 90;
}

} // namespace sfv
