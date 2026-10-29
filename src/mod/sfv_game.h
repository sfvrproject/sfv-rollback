#pragma once
// IGame over Street Fighter V's own rollback manager.
//
// Available now (from static analysis, to be confirmed by the probes):
//   - the snapshot region list, read from the manager's {ptr, size} table
//   - save/load of those regions, plus the game's post-restore callbacks
//   - restoring a frame held in the game's own ring (the plan's handle mode)
//   - per-region pointer masks (the state map) so hashes are portable
//
// Not available until phase 3 finds the hooks:
//   - BeginMatch: driving local Versus into a given MatchSetup
//   - ReadLocalInput / Advance: the per-player input slot in InputUnit and a
//     way to run exactly one battle tick on demand

#include "core/game.h"
#include "mod/image.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sfv::mod {

// True for a value that is an address in use in this process (heap, image):
// the AddressTest for state hashing (core/state.h).
bool LooksLikeAddress(uint64_t value);

// Which 8-byte words of each region hold pointers. Learned from local matches
// by mode=learn_statemap and shipped with the mod, so every peer masks the
// same words.
struct StateMap {
    struct Region {
        uint64_t size = 0;
        std::vector<uint8_t> ignore;
    };
    std::vector<Region> regions;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    uint64_t Fingerprint() const;
};

// Diagnostics: a write watch on a region (by its canonical name, "s884.0") of
// the next battle, from the moment its region list is built.
void WatchRegionOfNextBattle(const std::string& name, size_t offset, int length);
void ArmPendingRegionWatch(const std::vector<StateRegion>& regions, uintptr_t imageBase);

class SfvGame : public IGame {
public:
    explicit SfvGame(const GameImage& image) : image_(image) {}

    // Called from the RollbackUnit::Pre hook, on the game thread, every tick.
    void OnRollbackUnitTick(uint8_t* unit);

    uint8_t* Unit() const { return unit_; }
    uint8_t* Manager() const;

    // Re-reads the region table. True when the layout changed (new match).
    bool RefreshRegions();
    // KiwiGameSingleton's battle tick counter (outside the game's snapshot).
    uint8_t* SingletonTicks() const;
    // Increments whenever the layout changes; a driver bound to one battle
    // compares it to notice that battle ending.
    int LayoutVersion() const { return layoutVersion_; }
    // Sorts the regions again by their current contents. The order is fixed
    // when a battle's regions first appear, mid-setup; peers that sort again
    // at the same frame list them identically (for comparing dumps).
    void Recanonicalize()
    {
        FindPresentationState();
        rawLayout_.clear();
        RefreshRegions();
    }
    // Battle state the game keeps outside its snapshot that the battle ticks
    // advance (outside_probe.h): the pawns' and the camera's animation clocks.
    // Rolled back with the rest, else a rollback during the intro or the round
    // call leaves them ahead and the fight starts early on that machine.
    void FindPresentationState();
    void SetStateMap(const StateMap* map);

    // The InputUnit and BattleFrame objects, seen by their hooks. Their frame
    // counters are added to the snapshot as extra regions (they are missing
    // from the game's own list) when includeSyncCounters is set.
    void OnInputUnit(uint8_t* inputUnit);
    // A different BattleFrame object means a new battle: the unit pointers
    // seen so far belong to the old one and are dropped until seen again.
    void OnBattleFrame(uint8_t* battleFrame)
    {
        lastFrameMs_ = GetTickCount64();
        if (battleFrame != battleFrame_) {
            battleFrame_ = battleFrame;
            unit_ = nullptr;
            inputUnit_ = nullptr;
            battleFrameVersion_++;
        }
    }
    int BattleFrameVersion() const { return battleFrameVersion_; }
    // A new battle that reuses the old BattleFrame object: forget the units.
    void ForgetBattleObjects()
    {
        presentation_.clear();
        unit_ = nullptr;
        inputUnit_ = nullptr;
    }
    // A battle frame ran within the last half second.
    bool BattleRunning() const { return battleFrame_ && GetTickCount64() - lastFrameMs_ < 500; }
    // The battle simulation itself ticked (its InputUnit recorded a frame)
    // within the last half second; false while the pause menu is open.
    bool BattleTicking() const { return inputUnit_ && GetTickCount64() - lastInputMs_ < 500; }
    uint8_t* InputUnit() const { return inputUnit_; }
    uint8_t* BattleFrameObject() const { return battleFrame_; }
    bool includeSyncCounters = true;

    // Runs exactly one battle tick with these inputs (through the original
    // BattleFrame::Update), on the game thread, between frames.
    using BattleFrameFn = void (*)(uint8_t*);
    void SetBattleFrameFunction(BattleFrameFn fn) { runFrame_ = fn; }
    bool CanRunTicks() const { return runFrame_ && battleFrame_; }
    // Physical pad slot whose input is "the local player" (pad 0 = P1 slot).
    int localPadSlot = 0;
    std::atomic<int> inputLog{0}; // research: ticks to log the game's per-pad values for
    // When set, ReadLocalInput(p) returns pad p's own input (both local: tests).
    bool bothPadsLocal = false;
    // When set, the local player's input comes from here instead of what the
    // game computed from the pad (netplay isolates the pads from the game).
    PackedInput (*localInput)(int pad) = nullptr;
    // When set, both players' inputs come from here (a replay's stress run
    // feeds a recording's inputs through a stress session, replay_driver.h).
    std::function<PackedInput(int player)> inputOverride;
    // Set while a session owns the battle: only the ticks it runs may record
    // inputs (battle_driver.cpp).
    bool exclusiveTicks = false;

    // Words that differ between the first simulation of a frame and its
    // resimulation without affecting gameplay (engine-side transforms read by
    // gameplay code), keyed by region size and word offset so the mask holds
    // across matchups. Left out of hashes. Learned by "learnnoise".
    bool LoadNoise(const std::wstring& path);
    bool SaveNoise(const std::wstring& path) const;
    bool AddNoise(size_t regionSize, size_t offset); // true if new
    size_t NoiseWords() const
    {
        size_t n = 0;
        for (const auto& [size, offsets] : noise_) n += offsets.size();
        return n;
    }

    // Adds the words of the current region contents that look like pointers.
    void LearnPointers(StateMap& map) const;

    struct Inventory {
        size_t regionCount = 0;
        uint64_t stateBytes = 0;
        size_t groupCount = 0;
        uint64_t ringCount = 0;
        uint64_t ringMapSize = 0;
        game::FrameId currentId{-1, -1};
        size_t postRestoreCallbacks = 0;
    };
    Inventory Inspect() const;
    // Logs every region, group and callback (once per match).
    void LogLayout() const;

    // Matches won by each side since the last character select (the game's
    // WinCountLeft / WinCountRight), from the battle's round state; false
    // when no battle's regions are known.
    bool MatchWins(uint32_t& left, uint32_t& right) const;

    // The game's own Restore on a frame still held in its ring.
    bool RestoreRingFrame(const game::FrameId& id, bool discardNewer);

    // IGame
    std::string BuildId() const override;
    bool BeginMatch(const MatchSetup& setup) override;
    const std::vector<StateRegion>& Regions() override { return regions_; }
    PackedInput ReadLocalInput(int player) override;
    void Advance(const FrameInputs& inputs, bool rollingBack) override;
    void OnStateLoaded(int frame) override;
    bool OutcomeDecided() override;

private:
    void RunPostRestoreCallbacks(const game::FrameId& id) const;

    GameImage image_;
    uint8_t* unit_ = nullptr;
    uint8_t* inputUnit_ = nullptr;
    struct ExtraRegion {
        std::string name;
        uint8_t* ptr;
        size_t size;
    };
    std::vector<ExtraRegion> presentation_;
    uint64_t lastInputMs_ = 0;
    uint8_t* battleFrame_ = nullptr;
    BattleFrameFn runFrame_ = nullptr;
    uint32_t sampled_[4] = {}; // inputs the game computed for each pad, last tick
    uint32_t inject_[2] = {};
    bool injecting_ = false;
    std::vector<StateRegion> regions_;
    std::vector<std::pair<uint8_t*, size_t>> rawLayout_;
    int layoutVersion_ = 0;
    int battleFrameVersion_ = 0;
    std::atomic<uint64_t> lastFrameMs_{0};
    std::map<size_t, std::set<size_t>> noise_;
    const StateMap* stateMap_ = nullptr;
};

} // namespace sfv::mod
