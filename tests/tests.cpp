// End-to-end tests of the netplay driver, the .sfvr recorder and the desync
// tooling, using MockGame over the loopback network.

#include "core/lobby.h"
#include "core/match_code.h"
#include "core/mux.h"
#include "core/replay_diff.h"
#include "core/replay_file.h"
#include "core/spectate.h"
#include "core/state.h"
#include "core/verifier.h"
#include "testbed/match_runner.h"

#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>

using namespace sfv;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        g_checks++;                                                                                 \
        if (!(cond)) {                                                                              \
            g_failures++;                                                                           \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
        }                                                                                           \
    } while (0)

const char* RegionName(const ReplayFile& rf, size_t i) { return rf.regions[i].name.c_str(); }

bool HasRegion(const ReplayFile& rf, const std::vector<size_t>& regions, const char* name)
{
    for (size_t r : regions) {
        if (rf.regions[r].name == name) return true;
    }
    return false;
}

VerifyResult Verify(const ReplayFile& rf, MockBug bug, uint32_t machineValue, int stress)
{
    MockGameOptions go;
    go.bug = bug;
    go.machineValue = machineValue;
    MockGame game(go);
    VerifyOptions vo;
    vo.stressDistance = stress;
    return VerifyReplay(game, rf, vo);
}

void PrintVerify(const char* label, const VerifyResult& v, const ReplayFile& rf)
{
    std::printf("    verify %-22s %s at frame %d (%s)", label, ToString(v.status), v.frame, v.message.c_str());
    for (const auto& m : v.regions) std::printf(" [%s]", RegionName(rf, m.region));
    std::printf("\n");
}

int FirstDesync(const MockMatchResult& r)
{
    int first = -1;
    for (const auto& list : r.desyncs) {
        for (const auto& d : list) {
            if (first < 0 || d.frame < first) first = d.frame;
        }
    }
    return first;
}

void PrintMatch(const MockMatchResult& r)
{
    for (int p = 0; p < kNumPlayers; p++) {
        std::printf("    P%d: %d frames, final through %d, %u rollbacks (max %u frames), %u stalls, %d desync events\n",
                    p + 1, r.recording[p].FrameCount(), r.recording[p].finalThrough, r.rollbacks[p].total_count,
                    r.rollbacks[p].max_frames, r.rollbacks[p].total_stalls, r.desyncCount[p]);
    }
}

// ---------------------------------------------------------------------------

void TestHashes()
{
    CHECK(Hash64("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(Hash64("abc", 3) == 0x44BC2CF5AD770999ull);
    const char* longer = "Nobody inspects the spammish repetition";
    CHECK(Hash64(longer, std::strlen(longer)) == 0xFBCEA83C8A378BF1ull);
    CHECK(Crc32("123456789", 9) == 0xCBF43926u);
}

void TestIgnoreMask()
{
    uint8_t mem[24] = {};
    StateRegion r{"r", mem, sizeof(mem), {0x02}}; // ignore bytes 8..15
    std::vector<StateRegion> regions{r};
    uint64_t before, after;
    HashRegions(regions, &before);
    mem[9] = 0xFF; // inside the masked word: same hash
    HashRegions(regions, &after);
    CHECK(before == after);
    mem[16] = 0xFF; // outside: different hash
    HashRegions(regions, &after);
    CHECK(before != after);

    std::vector<uint8_t> a(24, 0), b(24, 0);
    b[9] = 1;
    b[20] = 1;
    const auto ranges = DiffStates({{"r", 24, {0x02}}}, a, b);
    CHECK(ranges.size() == 1 && ranges[0].offset == 16 && ranges[0].length == 8); // whole words
}

void TestReplayRoundTrip()
{
    ReplayFile rf;
    rf.modVersion = "0.1.0";
    rf.gameBuild = "mockfighter-1";
    rf.matchId = 42;
    rf.localPlayer = 1;
    rf.inputDelay = 2;
    rf.predictionWindow = 8;
    rf.setup.players[0].character = 7;
    rf.setup.stage = 3;
    rf.setup.seed = 0x1234567890ull;
    rf.regions = {{"a", 16, {0x02}}, {"b", 4, {}}, {"c", 1000, {}}};
    rf.initialHashes = {1, 2, 3};
    for (int f = 0; f < 500; f++) {
        // Region b changes every frame, c every 100: exercises the delta coding.
        const uint64_t h[3] = {1, 1000u + f, 3u + f / 100};
        rf.SetFrame(f, FrameInputs{{static_cast<PackedInput>(f), static_cast<PackedInput>(f * 3)}}, h);
    }
    rf.finalThrough = 490;
    rf.events.push_back({100, ReplayEventType::Rollback, 4, 0});
    rf.events.push_back({200, ReplayEventType::Desync, 0xAAAA, 0xBBBB});
    rf.dumps.push_back({200, "desync", std::vector<uint8_t>(1020, 0x5A)});

    const std::vector<uint8_t> bytes = SerializeReplay(rf);
    ReplayFile back;
    std::string error;
    CHECK(DeserializeReplay(bytes.data(), bytes.size(), &back, &error));
    CHECK(back.modVersion == rf.modVersion && back.gameBuild == rf.gameBuild);
    CHECK(back.matchId == 42 && back.localPlayer == 1 && back.finalThrough == 490);
    CHECK(back.setup == rf.setup);
    CHECK(back.regions == rf.regions);
    CHECK(back.inputs == rf.inputs);
    CHECK(back.hashes == rf.hashes);
    CHECK(back.initialHashes == rf.initialHashes);
    CHECK(back.events.size() == 2 && back.events[1].a == 0xAAAA && back.events[1].type == ReplayEventType::Desync);
    CHECK(back.dumps.size() == 1 && back.dumps[0].bytes == rf.dumps[0].bytes);
    // 500 frames x 3 regions would be 12000 bytes of hashes without delta coding.
    std::printf("    %zu bytes serialized\n", bytes.size());

    std::vector<uint8_t> corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x01;
    CHECK(!DeserializeReplay(corrupt.data(), corrupt.size(), &back, &error));
    CHECK(error.find("corrupt") != std::string::npos || error.find("malformed") != std::string::npos);

    CHECK(!DeserializeReplay(bytes.data(), bytes.size() - 6, &back, &error));
}

void TestCleanMatch()
{
    MockMatchOptions mo;
    mo.seed = 11;
    mo.link = {4, 3, 0.05};
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    CHECK(r.started);
    CHECK(FirstDesync(r) < 0);
    CHECK(r.rollbacks[0].total_count > 0 && r.rollbacks[1].total_count > 0);
    CHECK(r.recording[0].finalThrough > 2500);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.comparable && !d.setupDiffers && !d.initialStateDiffers);
    CHECK(d.firstInputMismatch < 0 && d.firstStateMismatch < 0);

    for (int p = 0; p < kNumPlayers; p++) {
        const auto straight = Verify(r.recording[p], MockBug::None, 0, 0);
        PrintVerify(p == 0 ? "P1 straight" : "P2 straight", straight, r.recording[p]);
        CHECK(straight.status == VerifyResult::Status::Match);
        CHECK(straight.framesChecked == r.recording[p].finalThrough + 1);
    }
    const auto stress = Verify(r.recording[0], MockBug::None, 0, 7);
    PrintVerify("P1 stress 7", stress, r.recording[0]);
    CHECK(stress.status == VerifyResult::Status::Match);

    // A report survives the trip through a file.
    const std::string path = "test_clean_p1.sfvr";
    std::string error;
    CHECK(SaveReplay(r.recording[0], path, &error));
    ReplayFile loaded;
    CHECK(LoadReplay(path, &loaded, &error));
    CHECK(loaded.hashes == r.recording[0].hashes && loaded.inputs == r.recording[0].inputs);
    std::remove(path.c_str());
}

void TestSetupMismatch()
{
    MockMatchOptions mo;
    mo.seed = 12;
    mo.renderedFrames = 600;
    mo.setup[1].players[0].character = 3; // P2's machine thinks P1 picked someone else
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    CHECK(FirstDesync(r) >= 0);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.setupDiffers);
    CHECK(d.initialStateDiffers);
}

void TestStateOutsideSnapshot()
{
    MockMatchOptions mo;
    mo.seed = 13;
    mo.link = {5, 3, 0.05};
    mo.bug[0] = mo.bug[1] = MockBug::StateOutsideSnapshot;
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    const int desync = FirstDesync(r);
    CHECK(desync >= 0);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.firstInputMismatch < 0 || d.firstInputMismatch > d.firstStateMismatch);
    CHECK(d.firstStateMismatch >= 0 && d.firstStateMismatch <= desync);
    CHECK(HasRegion(r.recording[0], d.regionsAtMismatch, "fighters"));

    // Neither recording is a straight simulation of its inputs, and resimulating
    // with the reported inputs exposes it.
    for (int p = 0; p < kNumPlayers; p++) {
        const auto v = Verify(r.recording[p], MockBug::StateOutsideSnapshot, 0, 7);
        PrintVerify(p == 0 ? "P1 stress 7" : "P2 stress 7", v, r.recording[p]);
        CHECK(v.status == VerifyResult::Status::Diverged || v.status == VerifyResult::Status::StressMismatch);
    }

    // On a clean recording the straight run passes and only rollback stress
    // finds the bug: the sync-test use of the verifier.
    MockMatchOptions clean = mo;
    clean.link = {0, 0, 0.0};
    const auto c = RunMockMatch(clean);
    CHECK(c.rollbacks[0].total_count == 0 && FirstDesync(c) < 0);
    const auto straight = Verify(c.recording[0], MockBug::StateOutsideSnapshot, 0, 0);
    const auto stress = Verify(c.recording[0], MockBug::StateOutsideSnapshot, 0, 7);
    PrintVerify("no-latency straight", straight, c.recording[0]);
    PrintVerify("no-latency stress 7", stress, c.recording[0]);
    CHECK(straight.status == VerifyResult::Status::Match);
    CHECK(stress.status == VerifyResult::Status::StressMismatch);
    CHECK(HasRegion(c.recording[0], [&] {
        std::vector<size_t> idx;
        for (const auto& m : stress.regions) idx.push_back(m.region);
        return idx;
    }(), "fighters"));
}

void TestMachineSpecificValue()
{
    MockMatchOptions mo;
    mo.seed = 14;
    mo.renderedFrames = 6000;
    mo.bug[0] = mo.bug[1] = MockBug::MachineSpecificValue;
    mo.machineValue[1] = 3; // only P2's machine is off
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    const int desync = FirstDesync(r);
    CHECK(desync >= 0);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(d.firstInputMismatch < 0 || d.firstInputMismatch > d.firstStateMismatch);
    CHECK(d.firstStateMismatch >= 0);
    CHECK(HasRegion(r.recording[0], d.regionsAtMismatch, "fighters"));
    CHECK(!d.bytesAtMismatch.empty());

    // The developer's machine (machineValue 0) reproduces P1 and not P2: the
    // nondeterminism is on P2's side, starting exactly at the diverging frame.
    const auto v1 = Verify(r.recording[0], MockBug::MachineSpecificValue, 0, 0);
    const auto v2 = Verify(r.recording[1], MockBug::MachineSpecificValue, 0, 0);
    PrintVerify("P1 straight", v1, r.recording[0]);
    PrintVerify("P2 straight", v2, r.recording[1]);
    CHECK(v1.status == VerifyResult::Status::Match);
    CHECK(v2.status == VerifyResult::Status::Diverged);
    CHECK(v2.frame == d.firstStateMismatch);
}

void TestEffectUsesGameplayRng()
{
    MockMatchOptions mo;
    mo.seed = 15;
    mo.link = {5, 3, 0.05};
    mo.bug[0] = mo.bug[1] = MockBug::EffectUsesGameplayRng;
    const auto r = RunMockMatch(mo);
    PrintMatch(r);
    CHECK(FirstDesync(r) >= 0);

    CHECK(r.reportsRaised[0] == 1 && r.reportsRaised[1] == 1);

    const DiffReport d = DiffReplays(r.recording[0], r.recording[1]);
    std::printf("%s", FormatDiff(d, r.recording[0]).c_str());
    CHECK(HasRegion(r.recording[0], d.regionsAtMismatch, "rng"));
    // Health checks arrive reordered under jitter; both peers must still end up
    // dumping the same, earliest desynced frame.
    CHECK(d.dumpFrame == d.firstStateMismatch);
    CHECK(!d.bytesAtMismatch.empty());

    const auto v = Verify(r.recording[0], MockBug::EffectUsesGameplayRng, 0, 7);
    PrintVerify("P1 stress 7", v, r.recording[0]);
    CHECK(v.status != VerifyResult::Status::Match);
}

int RunStress(MockBug bug, int frames)
{
    MockGameOptions go;
    go.bug = bug;
    go.input = [](int poll, int player) { return MasherInput(21, poll, player); };
    MockGame game(go);
    LoopbackNetwork unused(1, {});
    NetplayConfig nc;
    nc.stressCheckDistance = 7;
    NetplaySession session(game, unused.Endpoint(1), nc);
    if (!session.Start(nullptr)) return -2;
    for (int f = 0; f < frames; f++) session.Update();
    std::printf("    stress %d frames: next frame %d, %d desync events\n", frames, session.NextFrame(),
                session.DesyncCount());
    return session.DesyncCount();
}

void TestStressSession()
{
    CHECK(RunStress(MockBug::None, 1200) == 0);
    CHECK(RunStress(MockBug::EffectUsesGameplayRng, 1200) > 0);
    CHECK(RunStress(MockBug::StateOutsideSnapshot, 3000) > 0);
}

void TestMatchCodes()
{
    MatchCode room;
    room.kind = MatchCode::Kind::Room;
    room.room = "HWRH49";
    MatchCode back;
    CHECK(EncodeMatchCode(room) == "HWRH49");
    CHECK(DecodeMatchCode("HWRH49", &back) && back == room);
    CHECK(DecodeMatchCode(" w-hwr h49 ", &back) && back == room);
    CHECK(!DecodeMatchCode("HWRH40", &back)); // 0 is not in the room alphabet
    CHECK(!DecodeMatchCode("HWRH4", &back));
    CHECK(!DecodeMatchCode("HWRH49DD", &back)); // the old 8-character codes

    MatchCode udp{MatchCode::Kind::Udp, "", UdpAddress(0x7F000001, 47001), 0xBEEF};
    const std::string u = EncodeMatchCode(udp);
    CHECK(DecodeMatchCode(u, &back) && back == udp);
    std::printf("    udp code %s\n", u.c_str());

    // Case, dashes and look-alike letters do not matter; a typo is caught.
    std::string sloppy;
    for (char c : u) {
        if (c == '-') continue;
        sloppy += char(std::tolower(static_cast<unsigned char>(c == '0' ? 'O' : c == '1' ? 'I' : c)));
    }
    CHECK(DecodeMatchCode(sloppy, &back) && back == udp);
    std::string typo = u;
    typo[3] = typo[3] == 'A' ? 'B' : 'A';
    CHECK(!DecodeMatchCode(typo, &back));
    CHECK(!DecodeMatchCode("X-1234", &back));
}

void TestLobbyMessages()
{
    lobby::Message hello;
    hello.type = lobby::Type::Hello;
    hello.nonce = 77;
    hello.build = 0x1122334455667788ull;
    hello.role = lobby::Role::Spectator;
    hello.name = "Watcher";
    auto bytes = lobby::Encode(hello);
    auto back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->type == lobby::Type::Hello && back->nonce == 77 && back->build == hello.build &&
          back->role == lobby::Role::Spectator && back->name == "Watcher");

    // The session's state: seats with names, picks and flags.
    lobby::Message state;
    state.type = lobby::Type::SessionState;
    state.time = 123456;
    state.echo = 777;
    state.echoAge = 12;
    state.seats[0] = {"Host", true, true, {PackCode("Z37"), 2, 14, 1, 0, 5}, true, true, lobby::Choice::Rematch, 3};
    state.seats[1] = {"Joiner", true, false, {}, false, true, lobby::Choice::CharacterSelect, 1, 3, 59, lobby::Link::WiFi};
    state.phase = lobby::SessionPhase::PostMatch;
    state.played = 4;
    state.spectators = 2;
    state.matchId = 0xFEED;
    state.stage = 12;
    auto sb = lobby::Encode(state);
    back = lobby::Decode(sb.data(), sb.size());
    CHECK(back && back->time == 123456 && back->echo == 777 && back->echoAge == 12);
    CHECK(back && back->seats[0].name == "Host" && back->seats[0].pick == state.seats[0].pick &&
          back->seats[0].ready && back->seats[0].choice == lobby::Choice::Rematch && back->seats[0].wins == 3);
    CHECK(back && back->seats[1].name == "Joiner" && !back->seats[1].hasPick && back->seats[1].preloaded &&
          back->seats[1].choice == lobby::Choice::CharacterSelect);
    // Each player's own input delay, frame rate and link type (the HUD).
    CHECK(back && back->seats[1].inputDelay == 3 && back->seats[1].fps == 59 && back->seats[1].link == lobby::Link::WiFi &&
          back->seats[0].link == lobby::Link::Unknown);
    CHECK(back && back->phase == lobby::SessionPhase::PostMatch && back->played == 4 && back->spectators == 2 &&
          back->matchId == 0xFEED && back->stage == 12);

    // Spectator inputs: a full batch fits a Weyvelength datagram.
    lobby::Message inputs;
    inputs.type = lobby::Type::Inputs;
    inputs.matchId = 5;
    inputs.first = 600;
    for (size_t i = 0; i < lobby::kInputsPerMessage; i++) {
        inputs.frames.push_back({{{PackedInput(i), PackedInput(i * 3)}}, uint32_t(i * 7)});
    }
    auto ib = lobby::Encode(inputs);
    CHECK(ib.size() + 1 <= 1024);
    back = lobby::Decode(ib.data(), ib.size());
    CHECK(back && back->first == 600 && back->frames.size() == lobby::kInputsPerMessage &&
          back->frames[59].inputs.p[1] == 177 && back->frames[59].checksum == 413);

    lobby::Message welcome;
    welcome.type = lobby::Type::Welcome;
    welcome.matchId = 99;
    welcome.setup.players[0] = {PackCode("RYU"), 0, 2, 0, 1, 0};
    welcome.setup.players[1] = {PackCode("CMY"), 0, 1, 1, 0, 3};
    welcome.setup.stage = 7;
    welcome.setup.seed = 12345;
    bytes = lobby::Encode(welcome);
    back = lobby::Decode(bytes.data(), bytes.size());
    CHECK(back && back->matchId == 99 && back->setup == welcome.setup);
    CHECK(UnpackCode(back->setup.players[0].character) == "RYU");

    lobby::Message loaded;
    loaded.type = lobby::Type::Loaded;
    loaded.matchId = 99;
    loaded.stateChecksum = 0xABCD1234;
    loaded.reconciled = true;
    auto lb = lobby::Encode(loaded);
    back = lobby::Decode(lb.data(), lb.size());
    CHECK(back && back->stateChecksum == 0xABCD1234 && back->reconciled);

    // The frozen-state reconciliation messages.
    lobby::Message hashes;
    hashes.type = lobby::Type::StateHashes;
    hashes.matchId = 99;
    hashes.first = 100;
    hashes.total = 536;
    hashes.hashes = {1, 2, 0xFFFFFFFFFFFFFFFFull};
    auto hb = lobby::Encode(hashes);
    back = lobby::Decode(hb.data(), hb.size());
    CHECK(back && back->type == lobby::Type::StateHashes && back->first == 100 && back->total == 536 &&
          back->hashes == hashes.hashes);

    lobby::Message request;
    request.type = lobby::Type::StateRequest;
    request.regions = {3, 400};
    auto rb = lobby::Encode(request);
    back = lobby::Decode(rb.data(), rb.size());
    CHECK(back && back->regions == request.regions);

    lobby::Message data;
    data.type = lobby::Type::StateData;
    data.region = 400;
    data.offset = 1024;
    data.bytes.assign(lobby::kStateChunk, 0x5A);
    auto db = lobby::Encode(data);
    CHECK(db.size() + 1 <= 1024); // with the mux's channel byte
    back = lobby::Decode(db.data(), db.size());
    CHECK(back && back->region == 400 && back->offset == 1024 && back->bytes == data.bytes);
    CHECK(!lobby::Decode(db.data(), db.size() - 1)); // truncated

    bytes[0] ^= 1;
    CHECK(!lobby::Decode(bytes.data(), bytes.size()));
    CHECK(!lobby::Decode(bytes.data(), 5));
}

void TestMuxAndConditions()
{
    LoopbackNetwork net(3, {});
    Mux a(net.Endpoint(1)), b(net.Endpoint(2));
    const uint8_t x[] = {1, 2, 3};
    a.Channel(7).Send(2, x, 3);
    a.Channel(8).Send(2, x, 2);
    net.Tick();
    Packet p;
    CHECK(b.Channel(8).Receive(p) && p.data.size() == 2 && p.from == 1);
    CHECK(b.Channel(7).Receive(p) && p.data.size() == 3 && p.data[2] == 3);
    CHECK(!b.Channel(7).Receive(p));

    // 40 ms of added latency: nothing arrives at once, everything after.
    ConditionedTransport slow(net.Endpoint(3), 9);
    slow.SetConditions({40, 0, 0.0});
    for (int i = 0; i < 10; i++) slow.Send(4, x, 3);
    net.Tick();
    int got = 0;
    while (net.Endpoint(4).Receive(p)) got++;
    CHECK(got == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    slow.Receive(p); // flushes due packets
    net.Tick();
    while (net.Endpoint(4).Receive(p)) got++;
    CHECK(got == 10);
}

void TestWrongBuildIsRefused()
{
    MockMatchOptions mo;
    mo.seed = 16;
    mo.renderedFrames = 200;
    auto r = RunMockMatch(mo);
    r.recording[0].gameBuild = "some-other-build";
    const auto v = Verify(r.recording[0], MockBug::None, 0, 0);
    CHECK(v.status == VerifyResult::Status::Unsupported);
}

struct Test {
    const char* name;
    void (*fn)();
};

} // namespace

// A spectator fed the host's confirmed frames (out of order, in batches)
// plays the same match, and notices a frame whose checksum differs.
void TestSpectator()
{
    MockMatchOptions mo;
    mo.seed = 21;
    mo.renderedFrames = 1500;
    // No loss: the handshake's resends are timed in real milliseconds, and
    // the loopback match runs far faster than that.
    mo.link = {4, 2, 0.0};
    const auto r = RunMockMatch(mo);
    CHECK(r.started);
    const ReplayFile& host = r.recording[0];
    const int frames = host.finalThrough + 1;
    std::printf("    host recording: %d frames confirmed of %d\n", frames, host.FrameCount());
    CHECK(frames > 1000);

    for (int corrupt : {-1, 700}) {
        MockGame game;
        SpectateConfig sc;
        sc.setup = host.setup;
        sc.delayFrames = 10;
        SpectatorPlayback spec(game, sc);
        std::string error;
        CHECK(spec.Start(&error));
        // Batches of 60 in reverse order, each sent twice.
        for (int first = (frames - 1) / 60 * 60; first >= 0; first -= 60) {
            std::vector<FrameInputs> in;
            std::vector<uint32_t> sums;
            for (int f = first; f < std::min(frames, first + 60); f++) {
                in.push_back(host.inputs[f]);
                sums.push_back(host.FrameChecksum(f) ^ (f == corrupt ? 1u : 0u));
            }
            spec.Add(first, in, sums);
            spec.Add(first, in, sums);
        }
        CHECK(spec.NextWanted() == frames);
        for (int i = 0; i < frames * 2 && spec.NextFrame() < frames; i++) spec.Update();
        CHECK(spec.NextFrame() == frames);
        CHECK(spec.DesyncFrame() == corrupt);
        const ReplayFile rec = spec.Recording();
        CHECK(rec.FrameCount() == frames && rec.FrameChecksum(frames - 1) == host.FrameChecksum(frames - 1));
    }
}

int main()
{
    const Test tests[] = {
        {"hashes", TestHashes},
        {"ignore mask", TestIgnoreMask},
        {"replay file round trip", TestReplayRoundTrip},
        {"clean match", TestCleanMatch},
        {"setup mismatch", TestSetupMismatch},
        {"state outside snapshot", TestStateOutsideSnapshot},
        {"machine specific value", TestMachineSpecificValue},
        {"effect uses gameplay rng", TestEffectUsesGameplayRng},
        {"stress session", TestStressSession},
        {"match codes", TestMatchCodes},
        {"lobby messages", TestLobbyMessages},
        {"mux and net conditions", TestMuxAndConditions},
        {"wrong build refused", TestWrongBuildIsRefused},
        {"spectator", TestSpectator},
    };
    for (const auto& t : tests) {
        const int before = g_failures;
        const auto start = std::chrono::steady_clock::now();
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("[ %s ] %s (%lld ms)\n", g_failures == before ? " OK " : "FAIL", t.name,
                    static_cast<long long>(ms));
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
