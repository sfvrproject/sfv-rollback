#include "core/replay_diff.h"

#include "core/state.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sfv {

namespace {

std::string Format(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

} // namespace

namespace {

uint64_t Word(const std::vector<uint8_t>& s, size_t at)
{
    uint64_t v;
    std::memcpy(&v, &s[at], 8);
    return v;
}

// Offline there is no process to ask, so: a value in the range where 64-bit
// Windows puts heaps and images (above 1 TB; two packed int32 rarely get
// there). Addresses on both sides compare equal.
bool g_offlineAddresses = true;
bool PlausibleAddress(uint64_t v) { return g_offlineAddresses && v >= 0x10000000000ull && v < 0x800000000000ull; }

bool WordDiffers(const std::vector<uint8_t>& a, size_t ai, const std::vector<uint8_t>& b, size_t bi, size_t len)
{
    if (len < 8) return std::memcmp(&a[ai], &b[bi], len) != 0;
    const uint64_t va = Word(a, ai), vb = Word(b, bi);
    return va != vb && !(PlausibleAddress(va) && PlausibleAddress(vb));
}

size_t CountDiffering(const std::vector<uint8_t>& a, size_t ai, const std::vector<uint8_t>& b, size_t bi,
                      size_t size, const std::vector<uint8_t>& ignore)
{
    size_t n = 0;
    for (size_t w = 0; w < size; w += 8) {
        if (!IsIgnored(ignore, w) && WordDiffers(a, ai + w, b, bi + w, std::min<size_t>(8, size - w))) n++;
    }
    return n;
}

} // namespace

void SetDiffIgnoresAddresses(bool ignore) { g_offlineAddresses = ignore; }

std::vector<size_t> MatchRegions(const std::vector<RegionInfo>& regions, const std::vector<uint8_t>& a,
                                 const std::vector<uint8_t>& b)
{
    std::vector<size_t> offsets(regions.size());
    size_t base = 0;
    for (size_t r = 0; r < regions.size(); r++) {
        offsets[r] = base;
        base += static_cast<size_t>(regions[r].size);
    }
    std::vector<size_t> match(regions.size());
    for (size_t r = 0; r < regions.size(); r++) match[r] = r;
    if (base > a.size() || base > b.size()) return match;

    // Within each size, pair regions greedily by fewest differing words.
    size_t first = 0;
    while (first < regions.size()) {
        size_t last = first;
        while (last < regions.size() && regions[last].size == regions[first].size) last++;
        const size_t size = static_cast<size_t>(regions[first].size);
        struct Pair {
            size_t cost, i, j;
        };
        std::vector<Pair> pairs;
        for (size_t i = first; i < last; i++) {
            for (size_t j = first; j < last; j++) {
                pairs.push_back({CountDiffering(a, offsets[i], b, offsets[j], size, regions[i].ignore), i, j});
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.cost < y.cost; });
        std::vector<bool> usedA(last - first), usedB(last - first);
        for (const Pair& p : pairs) {
            if (usedA[p.i - first] || usedB[p.j - first]) continue;
            usedA[p.i - first] = usedB[p.j - first] = true;
            match[p.i] = p.j;
        }
        first = last;
    }
    return match;
}

std::vector<ByteRange> DiffStates(const std::vector<RegionInfo>& regions, const std::vector<uint8_t>& a,
                                  const std::vector<uint8_t>& b, size_t maxRanges)
{
    std::vector<ByteRange> out;
    const std::vector<size_t> match = MatchRegions(regions, a, b);
    std::vector<size_t> offsets(regions.size());
    size_t total = 0;
    for (size_t r = 0; r < regions.size(); r++) {
        offsets[r] = total;
        total += static_cast<size_t>(regions[r].size);
    }
    if (total > a.size() || total > b.size()) return out;

    for (size_t r = 0; r < regions.size() && out.size() < maxRanges; r++) {
        const size_t size = static_cast<size_t>(regions[r].size);
        const size_t ai = offsets[r], bi = offsets[match[r]];
        const auto& ignore = regions[r].ignore;
        auto differs = [&](size_t w) {
            return !IsIgnored(ignore, w) && WordDiffers(a, ai + w, b, bi + w, std::min<size_t>(8, size - w));
        };
        size_t w = 0;
        while (w < size && out.size() < maxRanges) {
            if (!differs(w)) {
                w += 8;
                continue;
            }
            size_t end = w;
            while (end < size && differs(end)) end += 8;
            ByteRange range;
            range.region = r;
            range.regionB = match[r];
            range.offset = w;
            range.length = std::min(end, size) - w;
            if (w + 8 <= size) {
                range.wordA = Word(a, ai + w);
                range.wordB = Word(b, bi + w);
            }
            out.push_back(range);
            w = end;
        }
    }
    return out;
}

DiffReport DiffReplays(const ReplayFile& a, const ReplayFile& b)
{
    DiffReport rep;

    if (a.gameBuild != b.gameBuild) {
        rep.comparable = false;
        rep.notes.push_back("different game builds: '" + a.gameBuild + "' vs '" + b.gameBuild + "'");
    }
    if (a.modVersion != b.modVersion) {
        rep.notes.push_back("different mod versions: " + a.modVersion + " vs " + b.modVersion);
    }
    if (a.matchId != b.matchId) {
        rep.notes.push_back("files come from different matches");
    }
    if (a.localPlayer >= 0 && a.localPlayer == b.localPlayer) {
        rep.notes.push_back("both files claim the same local player; expected one file per side");
    }
    if (a.regions != b.regions) {
        rep.comparable = false;
        rep.notes.push_back("state region layouts differ; hashes cannot be compared");
    }
    if (!(a.setup == b.setup)) {
        rep.setupDiffers = true;
        rep.notes.push_back("match setups differ: the peers started different matches");
    }
    if (!rep.comparable) return rep;

    const size_t regionCount = a.RegionCount();
    auto mismatchedRegions = [&](int frame) {
        std::vector<size_t> out;
        // A compact recording keeps region hashes only around its desyncs.
        if (!a.HasRegionHashes(frame) || !b.HasRegionHashes(frame)) {
            rep.notes.push_back("no region hashes for frame " + std::to_string(frame) +
                                " in both files (compact recordings keep them only around desyncs): "
                                "the differing regions are unknown");
            return out;
        }
        // A region mismatches when no region of the same size on the other
        // side has its hash (the peers' region orders may differ).
        const uint64_t* ha = a.FrameHashes(frame);
        const uint64_t* hb = b.FrameHashes(frame);
        for (size_t i = 0; i < regionCount; i++) {
            bool found = false;
            for (size_t j = 0; j < regionCount && !found; j++) {
                found = a.regions[j].size == a.regions[i].size && hb[j] == ha[i];
            }
            if (!found) out.push_back(i);
        }
        return out;
    };

    // The confirmed inputs are compared whatever the states do: they tell
    // the netcode's part apart from the game's.
    const int lastConfirmed = std::min({a.FrameCount() - 1, b.FrameCount() - 1, std::max(a.finalThrough, b.finalThrough)});
    rep.inputsComparedThrough = lastConfirmed;
    for (int f = 0; f <= lastConfirmed && rep.firstInputMismatch < 0; f++) {
        if (!(a.inputs[f] == b.inputs[f])) rep.firstInputMismatch = f;
    }

    rep.regionsAtMismatch = mismatchedRegions(-1);
    if (!rep.regionsAtMismatch.empty()) {
        rep.initialStateDiffers = true;
        rep.firstStateMismatch = -1;
        const StateDump* da = a.FindDump(-1);
        const StateDump* db = b.FindDump(-1);
        if (da && db) rep.bytesAtMismatch = DiffStates(a.regions, da->bytes, db->bytes);
        return rep;
    }

    const int last = std::min({a.FrameCount() - 1, b.FrameCount() - 1, std::max(a.finalThrough, b.finalThrough)});
    for (int f = 0; f <= last; f++) {
        if (rep.firstInputMismatch < 0 && !(a.inputs[f] == b.inputs[f])) rep.firstInputMismatch = f;
        if (rep.firstStateMismatch < 0 && a.FrameChecksum(f) != b.FrameChecksum(f)) {
            rep.firstStateMismatch = f;
            rep.regionsAtMismatch = mismatchedRegions(f);
        }
        if (rep.firstInputMismatch >= 0 && rep.firstStateMismatch >= 0) break;
    }

    if (rep.firstStateMismatch >= 0) {
        // Prefer dumps of the first diverging frame; otherwise the earliest
        // later frame both peers dumped still shows which bytes drifted.
        const StateDump *da = nullptr, *db = nullptr;
        for (const auto& d : a.dumps) {
            const StateDump* other = b.FindDump(d.frame);
            if (other && d.frame >= rep.firstStateMismatch && (!da || d.frame < da->frame)) {
                da = &d;
                db = other;
            }
        }
        if (da) {
            rep.dumpFrame = da->frame;
            rep.bytesAtMismatch = DiffStates(a.regions, da->bytes, db->bytes);
        }
    }

    const int finalA = a.finalThrough, finalB = b.finalThrough;
    if (rep.firstStateMismatch > std::min(finalA, finalB)) {
        rep.notes.push_back("the first mismatch lies after one side's last confirmed frame; it may be a prediction, "
                            "not a desync");
    }
    return rep;
}

std::string FormatDiff(const DiffReport& rep, const ReplayFile& a)
{
    std::string s;
    for (const auto& n : rep.notes) s += "note: " + n + "\n";
    if (!rep.comparable) return s + "result: not comparable\n";

    auto regionName = [&](size_t i) {
        return Format("#%zu %s (%" PRIu64 " bytes)", i, a.regions[i].name.c_str(), a.regions[i].size);
    };
    // "s128.0" or, when the peers listed it at different places, "s128.0~s128.3".
    auto pairName = [&](const ByteRange& br) {
        std::string n = a.regions[br.region].name;
        if (br.regionB != br.region) n += "~" + a.regions[br.regionB].name;
        return n;
    };

    if (rep.initialStateDiffers) {
        if (rep.firstInputMismatch >= 0) {
            s += Format("inputs: confirmed inputs differ from frame %d -> netcode/transport bug\n", rep.firstInputMismatch);
        }
        else if (rep.inputsComparedThrough >= 0) {
            s += Format("inputs: identical through frame %d\n", rep.inputsComparedThrough);
        }
        s += "result: the state before frame 0 differs -> match setup did not sync\n";
        for (size_t r : rep.regionsAtMismatch) s += "  region " + regionName(r) + "\n";
        if (!rep.bytesAtMismatch.empty()) s += "differing bytes in the starting state:\n";
        for (const auto& br : rep.bytesAtMismatch) {
            s += Format("  %-24s +0x%06zx  %zu byte(s)  %016" PRIx64 " vs %016" PRIx64 "\n",
                        pairName(br).c_str(), br.offset, br.length, br.wordA, br.wordB);
        }
        return s;
    }

    if (rep.firstInputMismatch >= 0) {
        s += Format("result: confirmed inputs differ from frame %d -> netcode/transport bug\n", rep.firstInputMismatch);
    }
    if (rep.firstStateMismatch < 0) {
        if (rep.firstInputMismatch < 0) s += "result: identical (inputs and state agree on every compared frame)\n";
        return s;
    }

    if (rep.firstInputMismatch < 0 || rep.firstInputMismatch > rep.firstStateMismatch) {
        s += Format("result: state diverges at frame %d with identical inputs -> nondeterminism\n",
                    rep.firstStateMismatch);
    }
    else {
        s += Format("state diverges at frame %d (after the input mismatch)\n", rep.firstStateMismatch);
    }
    for (size_t r : rep.regionsAtMismatch) s += "  region " + regionName(r) + "\n";

    if (!rep.bytesAtMismatch.empty()) {
        s += Format("differing bytes at frame %d (from state dumps):\n", rep.dumpFrame);
        for (const auto& br : rep.bytesAtMismatch) {
            s += Format("  %-24s +0x%06zx  %zu byte(s)  %016" PRIx64 " vs %016" PRIx64 "\n",
                        pairName(br).c_str(), br.offset, br.length, br.wordA, br.wordB);
        }
    }
    else {
        s += "no state dumps of that frame in both files; run `sfvr verify` on each file to find which side "
             "is reproducible\n";
    }
    return s;
}

} // namespace sfv
