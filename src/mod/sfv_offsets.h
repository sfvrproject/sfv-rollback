#pragma once
// Addresses in StreetFighterV/Binaries/Win64/StreetFighterV.exe, PE timestamp
// 0x648AD3A4 (2023-06-15). All are RVAs; the image is ASLR-relocated.
//
// Every hooked or called function has a 16-byte signature taken from the
// installed exe. The mod checks all of them before touching anything and stays
// inert on a different build.

#include <cstdint>

namespace sfv::game {

constexpr uint32_t kExeTimestamp = 0x648AD3A4;

struct CodeSite {
    const char* name;
    uint32_t rva;
    uint8_t bytes[16];
};

// RollbackUnit "Pre" task: opens a snapshot frame. Online it takes the frame id
// from the battle sync object (+0x217C/+0x2180); without one (offline) the id
// is -1 and no frame is opened, so the Store tasks do nothing.
constexpr CodeSite kRollbackUnitPre{
    "RollbackUnit::Pre", 0x486C40,
    {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x49, 0x8D, 0x6B, 0x88, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00}};
// bool InFreePlayPeriod(info): whether a character's free play (trial) period
// still runs, from two dates of the game's data compared with now (rva
// 0x583B30). A battle tick (rva 0x5971B0, a vtable slot at 0x2D58740) asks it
// every few seconds while its free-play flag (+0x430) is set and, once it
// says no, shows Dialog_INFO_0035 ("This character's free play period has
// ended. Returning to Main Menu.") and leaves the battle. Its only caller.
constexpr CodeSite kInFreePlayPeriod{
    "InFreePlayPeriod", 0x5A41C0,
    {0x40, 0x53, 0x56, 0x48, 0x83, 0xEC, 0x68, 0x48, 0x89, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48}};
// int BeginFrame(Manager*, const FrameId*)
constexpr CodeSite kManagerBeginFrame{
    "RollbackManager::BeginFrame", 0x48DC30,
    {0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83, 0x79, 0x70, 0x00}};
// void Store(Manager*, int group)
constexpr CodeSite kManagerStore{
    "RollbackManager::Store", 0x48DAA0,
    {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83, 0x79, 0x70, 0x00, 0x48, 0x8B, 0xF9, 0x0F, 0x84}};
// int Restore(Manager*, const FrameId*, int discardNewer). With discardNewer
// set it also drops every ring frame newer than the restored one.
constexpr CodeSite kManagerRestore{
    "RollbackManager::Restore", 0x48A310,
    {0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x79, 0x70, 0x00, 0x45}};
// Manager* GetManager(void* unitContext), called with RollbackUnit + 0xE8.
constexpr CodeSite kUnitGetManager{
    "RollbackUnit::GetManager", 0x308070,
    {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8D, 0x54, 0x24, 0x20, 0xE8}};
// First of the ten Store task thunks, for the signature check only.
constexpr CodeSite kStoreThunk1{
    "RollbackUnit::Store1", 0x477C60,
    {0x48, 0x83, 0xEC, 0x28, 0x80, 0xB9, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x74, 0x26, 0x80, 0xB9, 0x08}};

// InputUnit::RecordFrame(this, ...): once per battle tick, for each pad i
// that is local (this+0x100 bit i) stores this+0x118+4i, the pad's input value
// for this tick, into ring i (this+0x178 + i*0x800, 128 slots of {id, frame,
// value, valid}) at frame & 0x7F; for remote pads it repeats the last known
// value as a prediction (valid 0). Then frame++ (this+0x2184). The rings are in
// the game's snapshot; the counters (this+0x217C..+0x2188) are not.
constexpr CodeSite kInputUnitRecord{
    "InputUnit::RecordFrame", 0x43B310,
    {0x89, 0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0xB1, 0x7C, 0x21}};

// BattleFrame::Update(this): one rendered frame of battle. Runs scheduler
// phase 1, then 1 + [this+0x1A8] battle ticks (scheduler phase 2 each), then
// phases 4 and 8, and resets +0x1A8. The game's own rollback resimulates by
// setting +0x1A8. Called from the battle actor's Tick on the game thread.
constexpr CodeSite kBattleFrame{
    "BattleFrame::Update", 0x4DC6A0,
    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x22, 0xFC, 0xFF, 0xFF, 0x48, 0x8B}};

constexpr const CodeSite* kAllSites[] = {&kRollbackUnitPre, &kManagerBeginFrame, &kManagerStore,
                                         &kManagerRestore,  &kUnitGetManager,    &kStoreThunk1,
                                         &kInputUnitRecord, &kBattleFrame};

namespace inputunit {
constexpr uint32_t kPadCount = 0xF8;
constexpr uint32_t kLocalMask = 0x100;
constexpr uint32_t kValues = 0x118; // uint32 per pad
// Frame counters the game keeps outside its snapshot.
constexpr uint32_t kCountersBegin = 0x2170;
constexpr uint32_t kCountersSize = 0x30;
// Written at round start (rva 0x438EBD) from a global that differs between
// machines; forced during netplay ticks.
constexpr uint32_t kRoundParam = 0x2178;
// Reset counter that tags the input ring entries (rva 0x43C37C).
constexpr uint32_t kRingTag = 0x217C;
} // namespace inputunit

namespace singleton {
constexpr uint32_t kEngineGlobal = 0x3D878C8; // engine; +0x1E0 KiwiGameSingleton
constexpr uint32_t kSingleton = 0x1E0;
// A counter the battle ticks advance (outside_probe.h found it): not in the
// game's snapshot, so a rollback would leave it ahead.
constexpr uint32_t kTicks = 0x150;
constexpr uint32_t kTicksSize = 0x8;
} // namespace singleton

namespace presentation {
// Animation clocks (float seconds, then 1.0f) the battle ticks advance:
// KBP_BattlePawn_C and KBP_BattleViewTarget_C (outside_probe.h found them).
constexpr uint32_t kPawnClock = 0x9E0;
constexpr uint32_t kViewTargetClock = 0x5C0;
} // namespace presentation

namespace battleframe {
constexpr uint32_t kExtraTicks = 0x1A8; // ticks this frame = 1 + value, reset after
// Frame counter, pause and slow-motion state; not in the game's snapshot.
constexpr uint32_t kStateBegin = 0x190;
constexpr uint32_t kStateSize = 0x30;
} // namespace battleframe

// The 8-byte id of a snapshot frame: two int32 halves, both must be >= 0.
struct FrameId {
    int32_t lo;
    int32_t hi;
};

namespace unit {
constexpr uint32_t kManagerContext = 0xE8; // passed to GetManager
constexpr uint32_t kEnabled = 0xF8;        // byte: Store tasks run at all
constexpr uint32_t kInterval = 0x100;      // int: store every N frames (0 = every frame)
constexpr uint32_t kIntervalBase = 0x104;  // int
constexpr uint32_t kFrameOpen = 0x108;     // byte: Pre opened a frame this tick
constexpr uint32_t kFlag109 = 0x109;       // byte, gates the online path
constexpr uint32_t kFlag10A = 0x10A;       // byte, gates the online path
constexpr uint32_t kLastFrameId = 0x10C;   // FrameId
} // namespace unit

namespace manager {
constexpr uint32_t kRegionsBegin = 0x10;    // {void* ptr; size_t size}[]
constexpr uint32_t kRegionsEnd = 0x18;
constexpr uint32_t kPostStoreList = 0x28;   // std::list sentinel
constexpr uint32_t kPostRestoreList = 0x38; // std::list sentinel
constexpr uint32_t kRingMap = 0x58;         // std::deque<Entry> map: Entry*[]
constexpr uint32_t kRingMapSize = 0x60;     // power of two
constexpr uint32_t kRingOffset = 0x68;
constexpr uint32_t kRingCount = 0x70;       // number of frames held
constexpr uint32_t kCurrentId = 0x78;       // FrameId of the open frame
constexpr uint32_t kGroupsBegin = 0x80;     // Group[]
constexpr uint32_t kGroupsEnd = 0x88;

struct Region {
    uint8_t* ptr;
    uint64_t size;
};
// Regions [first, last] (inclusive) are copied to dst by Store(group).
struct Group {
    uint64_t first;
    uint64_t last;
    uint64_t unknown;
    uint8_t* dst;
};
// One ring frame: its id and the buffer the regions are copied into.
struct RingEntry {
    FrameId id;
    uint8_t* buffer;
};
// Post-restore callback node: called as fn(ctx, const FrameId*) when token != 0.
namespace callback {
constexpr uint32_t kNext = 0x00;
constexpr uint32_t kToken = 0x18;
constexpr uint32_t kContext = 0x28;
constexpr uint32_t kFunction = 0x30;
} // namespace callback
} // namespace manager

} // namespace sfv::game
