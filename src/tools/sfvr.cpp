// sfvr: inspect, compare and verify .sfvr match recordings / desync reports.
//
//   sfvr info    <file> [-v]
//   sfvr inputs  <file> [first] [last]
//   sfvr diff    <a.sfvr> <b.sfvr>
//   sfvr peers   <a.sfvr> <b.sfvr> [--initial | --pointers [--max-frame N]] [--raw] [--write noise.txt] [--show region]
//   sfvr verify  <file> [--stress N] [--every K] [--dump out.bin] [--mock-bug B] [--machine-value V]
//   sfvr export  <file> <out-dir>
//   sfvr demo    <out-dir>
//
// `verify` plays a recording back deterministically. Recordings made with the
// mock fighter verify here; Street Fighter V recordings are verified inside the
// game by the mod's playback mode, which runs the same verifier.

#include "core/replay_diff.h"
#include "core/replay_file.h"
#include "core/verifier.h"
#include "testbed/match_runner.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>

using namespace sfv;

namespace {

int Usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  sfvr info    <file> [-v]\n"
                 "  sfvr inputs  <file> [first] [last]\n"
                 "  sfvr diff    <a.sfvr> <b.sfvr>\n"
                 "  sfvr peers   <a.sfvr> <b.sfvr> [--initial | --pointers [--max-frame N]] [--raw] [--write noise.txt] [--show region]\n"
                 "  sfvr verify  <file> [--stress N] [--every K] [--dump out.bin]\n"
                 "               [--mock-bug none|outside-snapshot|machine-value|effect-rng] [--machine-value V]\n"
                 "  sfvr changes <file> <region size>\n"
                 "  sfvr export  <file> <out-dir>\n"
                 "  sfvr demo    <out-dir>\n");
    return 2;
}

bool Load(const char* path, ReplayFile& rf)
{
    std::string error;
    if (LoadReplay(path, &rf, &error)) return true;
    std::fprintf(stderr, "%s: %s\n", path, error.c_str());
    return false;
}

std::string InputText(PackedInput in)
{
    // Numpad notation for the direction, from P1-left perspective.
    int dir = 5;
    const bool up = in & kUp, down = in & kDown, left = in & kLeft, right = in & kRight;
    if (up) dir = left ? 7 : right ? 9 : 8;
    else if (down) dir = left ? 1 : right ? 3 : 2;
    else dir = left ? 4 : right ? 6 : 5;

    std::string s = std::to_string(dir);
    static const char* kButtons[] = {"LP", "MP", "HP", "LK", "MK", "HK"};
    for (int b = 0; b < 6; b++) {
        if (in & (kLP << b)) s += std::string("+") + kButtons[b];
    }
    // Bits outside the directions and six buttons, raw.
    if (const PackedInput other = in & ~PackedInput(0x3FF)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "+%x", other);
        s += buf;
    }
    return s;
}

const char* EventName(ReplayEventType t)
{
    switch (t) {
    case ReplayEventType::Rollback: return "rollback";
    case ReplayEventType::Desync: return "desync";
    case ReplayEventType::Disconnect: return "disconnect";
    case ReplayEventType::Note: return "note";
    }
    return "?";
}

int CmdInfo(int argc, char** argv)
{
    if (argc < 1) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;

    uint64_t stateBytes = 0;
    for (const auto& r : rf.regions) stateBytes += r.size;

    char when[64] = "?";
    const std::time_t t = static_cast<std::time_t>(rf.createdUnix);
    if (const std::tm* tm = std::gmtime(&t)) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", tm);

    std::printf("mod version     %s\n", rf.modVersion.c_str());
    std::printf("game build      %s\n", rf.gameBuild.c_str());
    std::printf("recorded        %s\n", when);
    std::printf("match id        %016" PRIx64 "\n", rf.matchId);
    std::printf("local player    %s\n", rf.localPlayer < 0 ? "-" : rf.localPlayer == 0 ? "P1" : "P2");
    std::printf("input delay     %u, prediction window %u\n", rf.inputDelay, rf.predictionWindow);
    for (int p = 0; p < kNumPlayers; p++) {
        const auto& ps = rf.setup.players[p];
        std::printf("P%d              character %s costume %u color %u V-Skill %u V-Trigger %u comment %u\n", p + 1,
                    UnpackCode(ps.character).c_str(), ps.costume, ps.color + 1, ps.vskill + 1, ps.vtrigger + 1,
                    ps.comment);
    }
    std::printf("match           stage %u.%u | rounds %u | timer %u | seed %016" PRIx64 "\n", rf.setup.stage,
                rf.setup.stageVariant, rf.setup.roundsToWin, rf.setup.timerSeconds, rf.setup.seed);
    std::printf("frames          %d recorded, confirmed through %d\n", rf.FrameCount(), rf.finalThrough);
    std::printf("state           %zu regions, %" PRIu64 " bytes\n", rf.RegionCount(), stateBytes);

    unsigned rollbacks = 0, maxRollback = 0, desyncs = 0;
    for (const auto& e : rf.events) {
        if (e.type == ReplayEventType::Rollback) {
            rollbacks++;
            maxRollback = std::max(maxRollback, e.a);
        }
        if (e.type == ReplayEventType::Desync) desyncs++;
    }
    std::printf("rollbacks       %u (longest %u frames)\n", rollbacks, maxRollback);
    std::printf("desync events   %u\n", desyncs);
    for (const auto& e : rf.events) {
        if (e.type == ReplayEventType::Desync) {
            std::printf("  desync at frame %d: local %08x remote %08x\n", e.frame, e.a, e.b);
        }
        else if (e.type == ReplayEventType::Disconnect) {
            std::printf("  disconnect after frame %d\n", e.frame);
        }
    }
    for (const auto& d : rf.dumps) {
        std::printf("state dump      frame %d (%s), %zu bytes\n", d.frame, d.label.c_str(), d.bytes.size());
    }

    if (verbose) {
        std::printf("\nregions:\n");
        for (size_t i = 0; i < rf.regions.size(); i++) {
            std::printf("  #%-4zu %-32s %10" PRIu64 " bytes\n", i, rf.regions[i].name.c_str(), rf.regions[i].size);
        }
        std::printf("\nevents:\n");
        for (const auto& e : rf.events) {
            std::printf("  frame %6d  %-10s %u %u\n", e.frame, EventName(e.type), e.a, e.b);
        }
    }
    return 0;
}

int CmdInputs(int argc, char** argv)
{
    if (argc < 1) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const int first = argc > 1 ? std::atoi(argv[1]) : 0;
    const int last = argc > 2 ? std::atoi(argv[2]) : rf.FrameCount() - 1;

    std::printf("frame   P1              P2              checksum\n");
    for (int f = std::max(0, first); f <= last && f < rf.FrameCount(); f++) {
        std::printf("%6d  %-15s %-15s %08x%s\n", f, InputText(rf.inputs[f].p[0]).c_str(),
                    InputText(rf.inputs[f].p[1]).c_str(), rf.FrameChecksum(f), f > rf.finalThrough ? "  (unconfirmed)" : "");
    }
    return 0;
}

int CmdDiff(int argc, char** argv)
{
    if (argc < 2) return Usage();
    ReplayFile a, b;
    if (!Load(argv[0], a) || !Load(argv[1], b)) return 1;
    const DiffReport d = DiffReplays(a, b);
    std::printf("%s", FormatDiff(d, a).c_str());
    return (d.comparable && d.firstInputMismatch < 0 && d.firstStateMismatch < 0 && !d.setupDiffers) ? 0 : 1;
}

// Words that differ between the two peers' dumps of the same frame (with
// identical inputs before it): values that only mean something in one process,
// such as truncated addresses, uninitialised buffers and engine clocks. With
// --write they are merged into a noise mask file (sfv_noise.txt format).
// How each region of one size lives through a match: on how many frames its
// hash changed, and the first and last such frame. A region that never
// changes is not touched by the simulation (an unused slot).
// Merges word offsets into a noise mask file (sfv_noise.txt format), keeping
// its comment lines.
int WriteNoise(const char* out, const std::map<uint64_t, std::set<uint64_t>>& words)
{
    // Merge with what the file already holds.
    std::map<uint64_t, std::set<uint64_t>> merged = words;
    std::vector<std::string> header;
    if (FILE* f = std::fopen(out, "r")) {
        char line[256];
        unsigned long long size = 0, offset = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (line[0] == '#') header.push_back(line);
            else if (std::sscanf(line, "%llu %llx", &size, &offset) == 2) merged[size].insert(offset & ~7ull);
        }
        std::fclose(f);
    }
    FILE* f = std::fopen(out, "w");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    for (const auto& h : header) std::fputs(h.c_str(), f);
    if (header.empty()) std::fputs("# <region size> <word offset, hex>\n", f);
    for (const auto& [size, offsets] : merged) {
        for (uint64_t o : offsets) std::fprintf(f, "%" PRIu64 " 0x%" PRIx64 "\n", size, o);
    }
    std::fclose(f);
    std::printf("wrote %s\n", out);
    return 0;
}

int CmdChanges(int argc, char** argv)
{
    if (argc < 2) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const uint64_t size = std::strtoull(argv[1], nullptr, 10);
    for (size_t i = 0; i < rf.regions.size(); i++) {
        if (rf.regions[i].size != size) continue;
        int changes = 0, first = -1, last = -1;
        const uint64_t* prev = rf.FrameHashes(-1);
        for (int f = 0; f < rf.FrameCount(); f++) {
            const uint64_t* h = rf.FrameHashes(f);
            if (h[i] != prev[i]) {
                changes++;
                if (first < 0) first = f;
                last = f;
            }
            prev = h;
        }
        std::printf("%-12s changed on %d of %d frames (first %d, last %d)\n", rf.regions[i].name.c_str(), changes,
                    rf.FrameCount(), first, last);
    }
    return 0;
}

int CmdLearnPeers(int argc, char** argv)
{
    if (argc < 2) return Usage();
    ReplayFile a, b;
    if (!Load(argv[0], a) || !Load(argv[1], b)) return 1;
    const char* out = nullptr;
    bool initialOnly = false; // learn only from the state before frame 0 (before any input mattered)
    // learn only words that hold an address on both sides, from every dump:
    // fields that are pointers (a live process may test a dangling one as data)
    bool pointersOnly = false;
    int maxFrame = INT32_MAX; // only dumps up to this frame (before any divergence)
    for (int k = 2; k < argc; k++) {
        if (!std::strcmp(argv[k], "--write") && k + 1 < argc) out = argv[k + 1];
        if (!std::strcmp(argv[k], "--initial")) initialOnly = true;
        if (!std::strcmp(argv[k], "--raw")) SetDiffIgnoresAddresses(false);
        if (!std::strcmp(argv[k], "--pointers")) pointersOnly = true;
        if (!std::strcmp(argv[k], "--max-frame") && k + 1 < argc) maxFrame = std::atoi(argv[k + 1]);
    }
    if (a.regions != b.regions) {
        std::fprintf(stderr, "the files have different region layouts\n");
        return 1;
    }

    std::map<uint64_t, std::set<uint64_t>> words; // region size -> word offsets
    if (pointersOnly) {
        auto plausible = [](uint64_t v) { return v >= 0x10000000000ull && v < 0x800000000000ull; };
        std::vector<size_t> offsets(a.regions.size());
        size_t total = 0;
        for (size_t r = 0; r < a.regions.size(); r++) {
            offsets[r] = total;
            total += static_cast<size_t>(a.regions[r].size);
        }
        for (const auto& da : a.dumps) {
            const StateDump* db = b.FindDump(da.frame);
            if (!db || da.frame > maxFrame || da.bytes.size() < total || db->bytes.size() < total) continue;
            const auto match = MatchRegions(a.regions, da.bytes, db->bytes);
            for (size_t r = 0; r < a.regions.size(); r++) {
                for (size_t w = 0; w + 8 <= a.regions[r].size; w += 8) {
                    uint64_t va, vb;
                    std::memcpy(&va, &da.bytes[offsets[r] + w], 8);
                    std::memcpy(&vb, &db->bytes[offsets[match[r]] + w], 8);
                    if (va != vb && plausible(va) && plausible(vb)) words[a.regions[r].size].insert(w);
                }
            }
        }
    }
    int frames = 0;
    std::map<std::string, size_t> firstCounts; // differing words per region in the first common dump
    for (const auto& da : a.dumps) {
        const StateDump* db = b.FindDump(da.frame);
        if (!db) continue;
        const auto ranges = DiffStates(a.regions, da.bytes, db->bytes, SIZE_MAX);
        std::map<std::string, size_t> counts;
        for (const auto& r : ranges) {
            for (size_t o = r.offset; o < r.offset + r.length; o += 8) {
                if (!pointersOnly && (!initialOnly || da.frame < 0)) words[a.regions[r.region].size].insert(o);
            }
            counts[a.regions[r.region].name] += (r.length + 7) / 8;
        }
        std::printf("frame %d (%s): %zu differing ranges\n", da.frame, da.label.c_str(), ranges.size());
        // What changed since the first dump: the divergence, not the noise.
        if (frames++ == 0) {
            firstCounts = counts;
            continue;
        }
        for (int k = 2; k + 1 < argc; k++) {
            if (std::strcmp(argv[k], "--show")) continue;
            for (const auto& r : ranges) {
                if (a.regions[r.region].name != argv[k + 1]) continue;
                std::printf("    %s+0x%zx (%zu bytes): %016" PRIx64 " vs %016" PRIx64 "\n", argv[k + 1], r.offset,
                            r.length, r.wordA, r.wordB);
            }
        }
        int shown = 0;
        for (const auto& [name, n] : counts) {
            const auto it = firstCounts.find(name);
            const size_t before = it == firstCounts.end() ? 0 : it->second;
            if (n != before && shown++ < 12) std::printf("    %-14s %zu words (was %zu)\n", name.c_str(), n, before);
        }
    }
    if (!frames) {
        std::fprintf(stderr, "no frame was dumped by both peers\n");
        return 1;
    }

    // Frame by frame from the per-region hashes: per region size, how many
    // regions of one peer have no equal on the other (orders may differ).
    // Changes in that count show when and where the states start to differ.
    {
        std::map<uint64_t, std::vector<size_t>> bySize;
        for (size_t i = 0; i < a.regions.size(); i++) bySize[a.regions[i].size].push_back(i);
        std::map<uint64_t, size_t> last;
        int changes = 0;
        const int lastFrame = std::min(a.FrameCount(), b.FrameCount()) - 1;
        for (int f = -1; f <= lastFrame && changes < 40; f++) {
            const uint64_t* ha = a.FrameHashes(f);
            const uint64_t* hb = b.FrameHashes(f);
            for (const auto& [size, idx] : bySize) {
                std::multiset<uint64_t> other;
                for (size_t i : idx) other.insert(hb[i]);
                size_t unmatched = 0;
                for (size_t i : idx) {
                    auto it = other.find(ha[i]);
                    if (it == other.end()) unmatched++;
                    else other.erase(it);
                }
                const auto prev = last.find(size);
                if (prev == last.end() ? unmatched == 0 : prev->second == unmatched) {
                    last[size] = unmatched;
                    continue;
                }
                if (f >= 0 && changes++ < 40) {
                    std::printf("  frame %5d: size %6" PRIu64 " x%zu: %zu differ (was %zu)\n", f, size, idx.size(),
                                unmatched, prev == last.end() ? 0 : prev->second);
                }
                last[size] = unmatched;
            }
        }
    }

    // Per peer on its own: the frames where each region size first changes,
    // to tell which side reached an event (round start, ...) first.
    for (int k = 2; k < argc; k++) {
        if (std::strcmp(argv[k], "--first-change")) continue;
        for (const ReplayFile* rf : {&a, &b}) {
            std::printf("first changes in %s:\n", rf == &a ? "the first file" : "the second file");
            std::map<uint64_t, std::vector<size_t>> bySize;
            for (size_t i = 0; i < rf->regions.size(); i++) bySize[rf->regions[i].size].push_back(i);
            for (const auto& [size, idx] : bySize) {
                const uint64_t* h0 = rf->FrameHashes(-1);
                for (int f = 0; f < rf->FrameCount(); f++) {
                    const uint64_t* h = rf->FrameHashes(f);
                    bool changed = false;
                    for (size_t i : idx) changed |= h[i] != h0[i];
                    if (!changed) continue;
                    if (f > 100) std::printf("  size %6" PRIu64 " x%zu: frame %d\n", size, idx.size(), f);
                    break;
                }
            }
        }
    }

    std::map<uint64_t, int> regionsOfSize;
    for (const auto& r : a.regions) regionsOfSize[r.size]++;
    size_t total = 0;
    for (const auto& [size, offsets] : words) {
        std::printf("  size %6" PRIu64 " x%-3d %4zu of %4" PRIu64 " words\n", size, regionsOfSize[size], offsets.size(),
                    (size + 7) / 8);
        total += offsets.size();
    }
    std::printf("%zu words in %zu region sizes\n", total, words.size());
    if (!out) return 0;

    return WriteNoise(out, words);
}

// Where a value sits in a state dump: every int32 equal to `value` in the dump
// of `frame`, by region and offset, with that word in the other dumps.
int CmdFind(int argc, char** argv)
{
    if (argc < 3) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const int frame = std::atoi(argv[1]);
    const int32_t value = static_cast<int32_t>(std::strtol(argv[2], nullptr, 0));
    const StateDump* dump = rf.FindDump(frame);
    if (!dump) {
        std::fprintf(stderr, "no dump of frame %d\n", frame);
        return 1;
    }
    std::vector<size_t> offsets(rf.regions.size());
    size_t total = 0;
    for (size_t r = 0; r < rf.regions.size(); r++) {
        offsets[r] = total;
        total += static_cast<size_t>(rf.regions[r].size);
    }
    for (size_t r = 0; r < rf.regions.size(); r++) {
        for (size_t o = 0; o + 4 <= rf.regions[r].size; o += 4) {
            int32_t v;
            std::memcpy(&v, &dump->bytes[offsets[r] + o], 4);
            if (v != value) continue;
            std::printf("%s+0x%zx:", rf.regions[r].name.c_str(), o);
            for (const auto& d : rf.dumps) {
                int32_t w;
                std::memcpy(&w, &d.bytes[offsets[r] + o], 4);
                std::printf("  [%d] %d", d.frame, w);
            }
            std::printf("\n");
        }
    }
    return 0;
}

// Words of a dump that point into a module ([base, base + size), for example
// the game's image): equal between two processes on one PC (one load address
// per boot), different between PCs. Per region size: offsets and how many.
int CmdImagePointers(int argc, char** argv)
{
    if (argc < 4) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const bool all = !std::strcmp(argv[1], "all");
    const int frame = std::atoi(argv[1]);
    const uint64_t base = std::strtoull(argv[2], nullptr, 16);
    const uint64_t size = std::strtoull(argv[3], nullptr, 16);
    const char* out = argc >= 6 && !std::strcmp(argv[4], "--write") ? argv[5] : nullptr;
    std::map<uint64_t, std::set<uint64_t>> offsets; // region size -> word offsets
    size_t words = 0, dumps = 0;
    for (const auto& dump : rf.dumps) {
        if (!all && dump.frame != frame) continue;
        dumps++;
        size_t at = 0;
        for (const auto& region : rf.regions) {
            for (size_t o = 0; o + 8 <= region.size && at + o + 8 <= dump.bytes.size(); o += 8) {
                uint64_t v;
                std::memcpy(&v, &dump.bytes[at + o], 8);
                if (v >= base && v < base + size && offsets[region.size].insert(o).second) words++;
            }
            at += static_cast<size_t>(region.size);
        }
    }
    if (!dumps) {
        std::fprintf(stderr, "no dump of frame %s\n", argv[1]);
        return 1;
    }
    for (const auto& [regionSize, set] : offsets) {
        std::printf("%6" PRIu64 ":", regionSize);
        for (uint64_t o : set) std::printf(" 0x%" PRIx64, o);
        std::printf("\n");
    }
    std::printf("%zu words in %zu region sizes point into the module (%zu dumps)\n", words, offsets.size(), dumps);
    return out ? WriteNoise(out, offsets) : 0;
}

// One region of a dump, word by word (int32 pairs and the 64-bit value).
int CmdRegion(int argc, char** argv)
{
    if (argc < 3) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const StateDump* dump = rf.FindDump(std::atoi(argv[1]));
    if (!dump) {
        std::fprintf(stderr, "no dump of frame %s\n", argv[1]);
        return 1;
    }
    size_t at = 0;
    for (const auto& region : rf.regions) {
        if (region.name == argv[2]) {
            for (size_t o = 0; o < region.size; o += 8) {
                uint64_t v = 0;
                std::memcpy(&v, &dump->bytes[at + o], std::min<size_t>(8, region.size - o));
                std::printf("  +0x%03zx  %016" PRIx64 "  %d %d\n", o, v, static_cast<int32_t>(v), static_cast<int32_t>(v >> 32));
            }
            return 0;
        }
        at += static_cast<size_t>(region.size);
    }
    std::fprintf(stderr, "no region %s\n", argv[2]);
    return 1;
}

// Writes the region table (regions.txt: name, size and offset in the dump) and
// every state dump (frame_<n>.bin) for scripts that look for fields.
int CmdExport(int argc, char** argv)
{
    if (argc < 2) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;
    const std::string dir = argv[1];
    FILE* table = std::fopen((dir + "/regions.txt").c_str(), "w");
    if (!table) {
        std::fprintf(stderr, "cannot write to %s\n", dir.c_str());
        return 1;
    }
    size_t at = 0;
    for (const auto& region : rf.regions) {
        std::fprintf(table, "%s %zu %zu\n", region.name.c_str(), static_cast<size_t>(region.size), at);
        at += static_cast<size_t>(region.size);
    }
    std::fclose(table);
    for (const auto& dump : rf.dumps) {
        const std::string path = dir + "/frame_" + std::to_string(dump.frame) + ".bin";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(dump.bytes.data(), 1, dump.bytes.size(), f);
            std::fclose(f);
        }
    }
    std::printf("%zu regions, %zu dumps\n", rf.regions.size(), rf.dumps.size());
    return 0;
}

bool ParseBug(const char* s, MockBug& out)
{
    if (!std::strcmp(s, "none")) out = MockBug::None;
    else if (!std::strcmp(s, "outside-snapshot")) out = MockBug::StateOutsideSnapshot;
    else if (!std::strcmp(s, "machine-value")) out = MockBug::MachineSpecificValue;
    else if (!std::strcmp(s, "effect-rng")) out = MockBug::EffectUsesGameplayRng;
    else return false;
    return true;
}

int CmdVerify(int argc, char** argv)
{
    if (argc < 1) return Usage();
    ReplayFile rf;
    if (!Load(argv[0], rf)) return 1;

    VerifyOptions vo;
    MockGameOptions go;
    const char* dumpPath = nullptr;
    for (int i = 1; i < argc; i++) {
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(argv[i], "--stress") && hasValue) vo.stressDistance = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--every") && hasValue) vo.stressEvery = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump") && hasValue) dumpPath = argv[++i];
        else if (!std::strcmp(argv[i], "--machine-value") && hasValue) go.machineValue = std::strtoul(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--mock-bug") && hasValue) {
            if (!ParseBug(argv[++i], go.bug)) return Usage();
        }
        else return Usage();
    }

    MockGame game(go);
    if (rf.gameBuild != game.BuildId()) {
        std::fprintf(stderr,
                     "%s was recorded on '%s'. Verify it in the game: set mode=playback and file=<path> in "
                     "sfv_rollback.ini and start local Versus.\n",
                     argv[0], rf.gameBuild.c_str());
        return 2;
    }

    const VerifyResult v = VerifyReplay(game, rf, vo);
    std::printf("%s: %s", ToString(v.status), v.message.c_str());
    if (v.status != VerifyResult::Status::Match && v.status != VerifyResult::Status::Unsupported) {
        std::printf(" (frame %d)", v.frame);
    }
    std::printf("\n");
    for (const auto& m : v.regions) {
        std::printf("  region #%zu %-24s expected %016" PRIx64 " got %016" PRIx64 "\n", m.region,
                    rf.regions[m.region].name.c_str(), m.expected, m.actual);
    }
    if (!v.referenceState.empty()) {
        for (const auto& br : DiffStates(rf.regions, v.referenceState, v.actualState)) {
            std::printf("  %-24s +0x%06zx  %zu byte(s) differ after resimulation\n",
                        rf.regions[br.region].name.c_str(), br.offset, br.length);
        }
    }
    if (const StateDump* dump = rf.FindDump(v.frame); dump && !v.actualState.empty()) {
        for (const auto& br : DiffStates(rf.regions, dump->bytes, v.actualState)) {
            std::printf("  %-24s +0x%06zx  %zu byte(s) differ from the recorded dump\n",
                        rf.regions[br.region].name.c_str(), br.offset, br.length);
        }
    }
    if (dumpPath && !v.actualState.empty()) {
        if (FILE* f = std::fopen(dumpPath, "wb")) {
            std::fwrite(v.actualState.data(), 1, v.actualState.size(), f);
            std::fclose(f);
            std::printf("wrote state of frame %d to %s\n", v.frame, dumpPath);
        }
    }
    return v.status == VerifyResult::Status::Match ? 0 : 1;
}

int CmdDemo(int argc, char** argv)
{
    if (argc < 1) return Usage();
    const std::filesystem::path dir = argv[0];
    std::filesystem::create_directories(dir);

    struct Scenario {
        const char* name;
        MockBug bug;
        uint32_t p2MachineValue;
    };
    const Scenario scenarios[] = {
        {"clean", MockBug::None, 0},
        {"machine-value", MockBug::MachineSpecificValue, 3},
        {"outside-snapshot", MockBug::StateOutsideSnapshot, 0},
        {"effect-rng", MockBug::EffectUsesGameplayRng, 0},
    };

    for (const auto& s : scenarios) {
        MockMatchOptions mo;
        mo.seed = 14;
        mo.renderedFrames = 6000;
        mo.link = {5, 3, 0.05};
        mo.bug[0] = mo.bug[1] = s.bug;
        mo.machineValue[1] = s.p2MachineValue;
        const MockMatchResult r = RunMockMatch(mo);
        for (int p = 0; p < kNumPlayers; p++) {
            const auto path = dir / (std::string(s.name) + (p == 0 ? "_p1.sfvr" : "_p2.sfvr"));
            std::string error;
            if (!SaveReplay(r.recording[p], path.string(), &error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("wrote %s (%d frames, %d desync events)\n", path.string().c_str(),
                        r.recording[p].FrameCount(), r.desyncCount[p]);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];
    if (cmd == "info") return CmdInfo(argc - 2, argv + 2);
    if (cmd == "inputs") return CmdInputs(argc - 2, argv + 2);
    if (cmd == "diff") return CmdDiff(argc - 2, argv + 2);
    if (cmd == "peers") return CmdLearnPeers(argc - 2, argv + 2);
    if (cmd == "changes") return CmdChanges(argc - 2, argv + 2);
    if (cmd == "find") return CmdFind(argc - 2, argv + 2);
    if (cmd == "images") return CmdImagePointers(argc - 2, argv + 2);
    if (cmd == "region") return CmdRegion(argc - 2, argv + 2);
    if (cmd == "export") return CmdExport(argc - 2, argv + 2);
    if (cmd == "verify") return CmdVerify(argc - 2, argv + 2);
    if (cmd == "demo") return CmdDemo(argc - 2, argv + 2);
    return Usage();
}
