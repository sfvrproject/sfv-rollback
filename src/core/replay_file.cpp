#include "core/replay_file.h"

#include "core/compress.h"
#include "core/state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Layout: "SFVR" u32 version, then chunks {u32 tag, u32 size, payload}, closed
// by an "END " chunk holding the CRC-32 of every byte before it. Unknown chunks
// are skipped so later versions can add data without breaking older tools.
// Version 3 (version 2 still reads): a checksum per frame (SUMS), region hashes
// for windows of frames (HWIN, in place of HASH: every frame), and any chunk
// may come compressed inside a PACK chunk.

namespace sfv {

namespace {

constexpr uint32_t Tag(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

constexpr uint32_t kMagic = Tag("SFVR");
constexpr uint32_t kMeta = Tag("META");
constexpr uint32_t kSetup = Tag("SETP");
constexpr uint32_t kRegions = Tag("RGNS");
constexpr uint32_t kInitial = Tag("INIT");
constexpr uint32_t kInputs = Tag("INPT");
constexpr uint32_t kHashes = Tag("HASH"); // version 2
constexpr uint32_t kChecksums = Tag("SUMS");
constexpr uint32_t kHashWindows = Tag("HWIN");
constexpr uint32_t kPack = Tag("PACK");
constexpr uint32_t kEvents = Tag("EVNT");
constexpr uint32_t kDump = Tag("DUMP");
constexpr uint32_t kEnd = Tag("END ");

class Writer {
public:
    std::vector<uint8_t> buf;

    void Raw(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void U8(uint8_t v) { Raw(&v, 1); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void I32(int32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void Str(const std::string& s)
    {
        U32(static_cast<uint32_t>(s.size()));
        Raw(s.data(), s.size());
    }

    size_t BeginChunk(uint32_t tag)
    {
        U32(tag);
        U32(0);
        return buf.size();
    }
    void EndChunk(size_t start)
    {
        const uint32_t size = static_cast<uint32_t>(buf.size() - start);
        std::memcpy(&buf[start - 4], &size, 4);
    }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    bool ok() const { return ok_; }
    size_t left() const { return static_cast<size_t>(end_ - p_); }
    const uint8_t* pos() const { return p_; }

    bool Raw(void* out, size_t n)
    {
        if (!ok_ || left() < n) return ok_ = false;
        std::memcpy(out, p_, n);
        p_ += n;
        return true;
    }
    template <typename T> T Get()
    {
        T v{};
        Raw(&v, sizeof(T));
        return v;
    }
    std::string Str()
    {
        const uint32_t n = Get<uint32_t>();
        if (!ok_ || left() < n) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    bool Skip(size_t n)
    {
        if (left() < n) return ok_ = false;
        p_ += n;
        return true;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

void WriteSetup(Writer& w, const MatchSetup& s)
{
    for (const auto& p : s.players) {
        w.U32(p.character);
        w.U16(p.costume);
        w.U8(p.color);
        w.U8(p.vskill);
        w.U8(p.vtrigger);
        w.U8(p.comment);
    }
    w.U16(s.stage);
    w.U8(s.stageVariant);
    w.U8(s.roundsToWin);
    w.U16(s.timerSeconds);
    w.U64(s.seed);
    // After the fixed part, so older files (without it) still read.
    for (const auto& p : s.players) w.U8(p.variation);
    for (int i = 0; i < kNumPlayers; i++) w.U16(s.wins[i]);
    for (int i = 0; i < kNumPlayers; i++) w.U16(s.streaks[i]);
}

void ReadSetup(Reader& r, MatchSetup& s)
{
    for (auto& p : s.players) {
        p.character = r.Get<uint32_t>();
        p.costume = r.Get<uint16_t>();
        p.color = r.Get<uint8_t>();
        p.vskill = r.Get<uint8_t>();
        p.vtrigger = r.Get<uint8_t>();
        p.comment = r.Get<uint8_t>();
    }
    s.stage = r.Get<uint16_t>();
    s.stageVariant = r.Get<uint8_t>();
    s.roundsToWin = r.Get<uint8_t>();
    s.timerSeconds = r.Get<uint16_t>();
    s.seed = r.Get<uint64_t>();
    if (r.left() >= kNumPlayers) {
        for (auto& p : s.players) p.variation = r.Get<uint8_t>();
    }
    if (r.left() >= 4 * kNumPlayers) {
        for (int i = 0; i < kNumPlayers; i++) s.wins[i] = r.Get<uint16_t>();
        for (int i = 0; i < kNumPlayers; i++) s.streaks[i] = r.Get<uint16_t>();
    }
}

// Most regions do not change every frame, so each frame stores a bitmap of the
// regions whose hash differs from the previous frame and only those hashes.
// `prev` holds the row before `first` and ends as the row of the last frame.
void WriteHashRows(Writer& w, const ReplayFile& rf, int first, int count, std::vector<uint64_t>& prev)
{
    const size_t regions = rf.RegionCount();
    std::vector<uint8_t> bitmap((regions + 7) / 8);
    for (int f = first; f < first + count; f++) {
        const uint64_t* row = rf.FrameHashes(f);
        std::fill(bitmap.begin(), bitmap.end(), uint8_t(0));
        for (size_t i = 0; i < regions; i++) {
            if (row[i] != prev[i]) bitmap[i / 8] |= uint8_t(1u << (i % 8));
        }
        w.Raw(bitmap.data(), bitmap.size());
        for (size_t i = 0; i < regions; i++) {
            if (row[i] != prev[i]) {
                w.U64(row[i]);
                prev[i] = row[i];
            }
        }
    }
}

bool ReadHashRows(Reader& r, ReplayFile& rf, int first, int count, std::vector<uint64_t>& prev)
{
    const size_t regions = rf.RegionCount();
    std::vector<uint8_t> bitmap((regions + 7) / 8);
    for (int f = first; f < first + count && r.ok(); f++) {
        r.Raw(bitmap.data(), bitmap.size());
        for (size_t i = 0; i < regions; i++) {
            if (bitmap[i / 8] & (1u << (i % 8))) prev[i] = r.Get<uint64_t>();
        }
        std::memcpy(&rf.hashes[size_t(f) * regions], prev.data(), regions * sizeof(uint64_t));
    }
    return r.ok();
}

// Version 2: every frame's region hashes, from the state before frame 0 on.
bool ReadHashes(Reader& r, ReplayFile& rf, std::string* error)
{
    const uint32_t frames = r.Get<uint32_t>();
    const uint32_t regions = r.Get<uint32_t>();
    if (regions != rf.RegionCount() || frames != static_cast<uint32_t>(rf.FrameCount())) {
        if (error) *error = "hash table does not match the region or frame count";
        return false;
    }
    std::vector<uint64_t> prev = rf.initialHashes;
    prev.resize(regions, 0);
    rf.hashes.assign(size_t(frames) * regions, 0);
    return ReadHashRows(r, rf, 0, static_cast<int>(frames), prev);
}

// The frames a file keeps region hashes for (see ReplayDetail).
std::vector<uint8_t> KeptFrames(const ReplayFile& rf, ReplayDetail detail)
{
    const int frames = rf.FrameCount();
    std::vector<uint8_t> keep(size_t(frames), 0);
    auto mark = [&](int from, int to) {
        for (int f = std::max(0, from); f <= std::min(to, frames - 1); f++) keep[f] = rf.HasRegionHashes(f) ? 1 : 0;
    };
    if (detail == ReplayDetail::Full) mark(0, frames - 1);
    else {
        for (const auto& e : rf.events) {
            if (e.type == ReplayEventType::Desync) mark(e.frame - kDesyncHashesBefore, e.frame + kDesyncHashesAfter);
        }
    }
    return keep;
}

// Version 3: runs of consecutive kept frames, each starting from a zero row
// (its first frame stores every region).
void WriteHashWindows(Writer& w, const ReplayFile& rf, const std::vector<uint8_t>& keep)
{
    const int frames = rf.FrameCount();
    std::vector<std::pair<int, int>> windows;
    for (int f = 0; f < frames; f++) {
        if (!keep[f]) continue;
        if (!windows.empty() && windows.back().first + windows.back().second == f) windows.back().second++;
        else windows.push_back({f, 1});
    }
    w.U32(static_cast<uint32_t>(frames));
    w.U32(static_cast<uint32_t>(rf.RegionCount()));
    w.U32(static_cast<uint32_t>(windows.size()));
    for (const auto& [first, count] : windows) {
        w.I32(first);
        w.U32(static_cast<uint32_t>(count));
        std::vector<uint64_t> prev(rf.RegionCount(), 0);
        WriteHashRows(w, rf, first, count, prev);
    }
}

bool ReadHashWindows(Reader& r, ReplayFile& rf, std::string* error)
{
    const uint32_t frames = r.Get<uint32_t>();
    const uint32_t regions = r.Get<uint32_t>();
    const uint32_t windows = r.Get<uint32_t>();
    if (regions != rf.RegionCount() || frames != static_cast<uint32_t>(rf.FrameCount())) {
        if (error) *error = "hash windows do not match the region or frame count";
        return false;
    }
    rf.hashes.assign(size_t(frames) * regions, 0);
    rf.hashesKept.assign(frames, 0);
    for (uint32_t k = 0; k < windows && r.ok(); k++) {
        const int32_t first = r.Get<int32_t>();
        const uint32_t count = r.Get<uint32_t>();
        if (first < 0 || count > frames || uint32_t(first) > frames - count) {
            if (error) *error = "hash window outside the recording";
            return false;
        }
        std::vector<uint64_t> prev(regions, 0);
        if (!ReadHashRows(r, rf, first, static_cast<int>(count), prev)) break;
        std::fill(rf.hashesKept.begin() + first, rf.hashesKept.begin() + first + count, uint8_t(1));
    }
    return r.ok();
}

// A chunk written from `body`, compressed (in a PACK chunk: the tag, the
// size before compression, the compressed bytes) when that saves a tenth.
void PutChunk(Writer& out, uint32_t tag, const Writer& body)
{
    std::vector<uint8_t> packed;
    if (body.buf.size() >= 512 && Compress(body.buf.data(), body.buf.size(), &packed) &&
        packed.size() + 8 < body.buf.size() - body.buf.size() / 10) {
        const size_t c = out.BeginChunk(kPack);
        out.U32(tag);
        out.U32(static_cast<uint32_t>(body.buf.size()));
        out.Raw(packed.data(), packed.size());
        out.EndChunk(c);
        return;
    }
    const size_t c = out.BeginChunk(tag);
    out.Raw(body.buf.data(), body.buf.size());
    out.EndChunk(c);
}

} // namespace

bool ReplayFile::HasRegionHashes(int frame) const
{
    if (frame < 0) return true;
    if (frame >= FrameCount()) return false;
    return hashesKept.empty() || hashesKept[frame];
}

const uint64_t* ReplayFile::FrameHashes(int frame) const
{
    if (frame < 0) return initialHashes.data();
    return &hashes[size_t(frame) * RegionCount()];
}

uint32_t ReplayFile::FrameChecksum(int frame) const
{
    if (HasRegionHashes(frame)) return FoldChecksum(FrameHashes(frame), RegionCount());
    return size_t(frame) < checksums.size() ? checksums[frame] : 0;
}

void ReplayFile::SetFrame(int frame, const FrameInputs& in, const uint64_t* regionHashes)
{
    if (frame < 0) return;
    const size_t regionCount = RegionCount();
    if (frame >= FrameCount()) {
        inputs.resize(size_t(frame) + 1);
        hashes.resize((size_t(frame) + 1) * regionCount, 0);
        if (!hashesKept.empty()) {
            hashesKept.resize(size_t(frame) + 1, 0);
            checksums.resize(size_t(frame) + 1, 0);
        }
    }
    inputs[frame] = in;
    std::memcpy(&hashes[size_t(frame) * regionCount], regionHashes, regionCount * sizeof(uint64_t));
    if (!hashesKept.empty()) hashesKept[frame] = 1;
}

const StateDump* ReplayFile::FindDump(int frame) const
{
    for (const auto& d : dumps) {
        if (d.frame == frame) return &d;
    }
    return nullptr;
}

std::vector<uint8_t> SerializeReplay(const ReplayFile& rf, ReplayDetail detail)
{
    Writer w;
    w.U32(kMagic);
    w.U32(kReplayFormatVersion);

    Writer b;
    b.Str(rf.modVersion);
    b.Str(rf.gameBuild);
    b.U64(rf.matchId);
    b.I32(rf.localPlayer);
    b.U64(rf.createdUnix);
    b.U32(rf.inputDelay);
    b.U32(rf.predictionWindow);
    b.I32(rf.finalThrough);
    PutChunk(w, kMeta, b);

    b = {};
    WriteSetup(b, rf.setup);
    PutChunk(w, kSetup, b);

    b = {};
    b.U32(static_cast<uint32_t>(rf.regions.size()));
    for (const auto& r : rf.regions) {
        b.Str(r.name);
        b.U64(r.size);
        b.U32(static_cast<uint32_t>(r.ignore.size()));
        b.Raw(r.ignore.data(), r.ignore.size());
    }
    PutChunk(w, kRegions, b);

    b = {};
    b.U32(static_cast<uint32_t>(rf.initialHashes.size()));
    for (uint64_t h : rf.initialHashes) b.U64(h);
    PutChunk(w, kInitial, b);

    b = {};
    b.U32(static_cast<uint32_t>(rf.inputs.size()));
    for (const auto& in : rf.inputs) {
        for (PackedInput p : in.p) b.U32(p);
    }
    PutChunk(w, kInputs, b);

    b = {};
    b.U32(static_cast<uint32_t>(rf.FrameCount()));
    for (int f = 0; f < rf.FrameCount(); f++) b.U32(rf.FrameChecksum(f));
    PutChunk(w, kChecksums, b);

    b = {};
    WriteHashWindows(b, rf, KeptFrames(rf, detail));
    PutChunk(w, kHashWindows, b);

    b = {};
    b.U32(static_cast<uint32_t>(rf.events.size()));
    for (const auto& e : rf.events) {
        b.I32(e.frame);
        b.U16(static_cast<uint16_t>(e.type));
        b.U16(0);
        b.U32(e.a);
        b.U32(e.b);
    }
    PutChunk(w, kEvents, b);

    for (const auto& d : rf.dumps) {
        b = {};
        b.I32(d.frame);
        b.Str(d.label);
        b.U32(static_cast<uint32_t>(d.bytes.size()));
        b.Raw(d.bytes.data(), d.bytes.size());
        PutChunk(w, kDump, b);
    }

    const uint32_t crc = Crc32(w.buf.data(), w.buf.size());
    const size_t c = w.BeginChunk(kEnd);
    w.U32(crc);
    w.EndChunk(c);
    return std::move(w.buf);
}

bool DeserializeReplay(const uint8_t* data, size_t len, ReplayFile* out, std::string* error)
{
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };

    Reader r(data, len);
    if (r.Get<uint32_t>() != kMagic) return fail("not an .sfvr file");
    const uint32_t version = r.Get<uint32_t>();
    if (version != 2 && version != kReplayFormatVersion) return fail("unsupported .sfvr format version");

    ReplayFile rf;
    bool sawEnd = false, sawInputs = false, sawHashes = false;

    // One chunk's payload; a PACK chunk's comes here once uncompressed.
    auto handle = [&](uint32_t tag, Reader& c) {
        switch (tag) {
        case kMeta:
            rf.modVersion = c.Str();
            rf.gameBuild = c.Str();
            rf.matchId = c.Get<uint64_t>();
            rf.localPlayer = c.Get<int32_t>();
            rf.createdUnix = c.Get<uint64_t>();
            rf.inputDelay = c.Get<uint32_t>();
            rf.predictionWindow = c.Get<uint32_t>();
            rf.finalThrough = c.Get<int32_t>();
            break;
        case kSetup:
            ReadSetup(c, rf.setup);
            break;
        case kRegions: {
            const uint32_t n = c.Get<uint32_t>();
            for (uint32_t i = 0; i < n && c.ok(); i++) {
                RegionInfo info;
                info.name = c.Str();
                info.size = c.Get<uint64_t>();
                const uint32_t maskLen = c.Get<uint32_t>();
                if (maskLen > c.left()) return fail("bad region mask");
                info.ignore.resize(maskLen);
                c.Raw(info.ignore.data(), maskLen);
                rf.regions.push_back(std::move(info));
            }
            break;
        }
        case kInitial: {
            const uint32_t n = c.Get<uint32_t>();
            if (n > c.left() / 8) return fail("bad initial hash count");
            rf.initialHashes.resize(n);
            c.Raw(rf.initialHashes.data(), n * sizeof(uint64_t));
            break;
        }
        case kInputs: {
            const uint32_t n = c.Get<uint32_t>();
            if (n > c.left() / (sizeof(PackedInput) * kNumPlayers)) return fail("bad input count");
            rf.inputs.resize(n);
            for (auto& in : rf.inputs) {
                for (auto& p : in.p) p = c.Get<uint32_t>();
            }
            sawInputs = true;
            break;
        }
        case kChecksums: {
            const uint32_t n = c.Get<uint32_t>();
            if (n > c.left() / 4) return fail("bad checksum count");
            rf.checksums.resize(n);
            c.Raw(rf.checksums.data(), n * sizeof(uint32_t));
            break;
        }
        case kHashes:
        case kHashWindows:
            // Need the region table and the inputs, which are written first.
            if (!sawInputs) return fail("hash chunk before input chunk");
            if (!(tag == kHashes ? ReadHashes(c, rf, error) : ReadHashWindows(c, rf, error))) return false;
            sawHashes = true;
            break;
        case kEvents: {
            const uint32_t n = c.Get<uint32_t>();
            for (uint32_t i = 0; i < n && c.ok(); i++) {
                ReplayEvent e;
                e.frame = c.Get<int32_t>();
                e.type = static_cast<ReplayEventType>(c.Get<uint16_t>());
                c.Get<uint16_t>();
                e.a = c.Get<uint32_t>();
                e.b = c.Get<uint32_t>();
                rf.events.push_back(e);
            }
            break;
        }
        case kDump: {
            StateDump d;
            d.frame = c.Get<int32_t>();
            d.label = c.Str();
            const uint32_t n = c.Get<uint32_t>();
            if (n > c.left()) return fail("bad dump size");
            d.bytes.resize(n);
            c.Raw(d.bytes.data(), n);
            rf.dumps.push_back(std::move(d));
            break;
        }
        default:
            break; // newer chunk type
        }
        if (!c.ok()) return fail("malformed chunk");
        return true;
    };

    while (r.ok() && r.left() > 0) {
        const size_t tagOffset = static_cast<size_t>(r.pos() - data);
        const uint32_t tag = r.Get<uint32_t>();
        const uint32_t size = r.Get<uint32_t>();
        if (!r.ok() || r.left() < size) return fail("truncated chunk");
        Reader c(r.pos(), size);
        r.Skip(size);

        if (tag == kEnd) {
            if (c.Get<uint32_t>() != Crc32(data, tagOffset)) return fail("checksum mismatch: file is corrupt");
            sawEnd = true;
            break;
        }
        if (tag == kPack) {
            const uint32_t inner = c.Get<uint32_t>();
            const uint32_t rawSize = c.Get<uint32_t>();
            std::vector<uint8_t> raw;
            if (!c.ok() || inner == kPack || !Decompress(c.pos(), c.left(), rawSize, &raw)) {
                return fail("cannot uncompress a chunk: file is corrupt");
            }
            Reader rc(raw.data(), raw.size());
            if (!handle(inner, rc)) return false;
        }
        else if (!handle(tag, c)) {
            return false;
        }
    }

    if (!sawEnd) return fail("file is truncated (no END chunk)");
    if (!sawHashes || rf.hashes.size() != size_t(rf.FrameCount()) * rf.RegionCount()) return fail("missing hash chunk");
    if (rf.initialHashes.size() != rf.RegionCount()) return fail("initial hashes do not match region table");
    if (!rf.hashesKept.empty()) {
        if (std::all_of(rf.hashesKept.begin(), rf.hashesKept.end(), [](uint8_t k) { return k != 0; })) {
            // A full file: like a live recording.
            rf.hashesKept.clear();
            rf.checksums.clear();
        }
        else if (rf.checksums.size() != size_t(rf.FrameCount())) {
            return fail("missing checksums for the frames without region hashes");
        }
    }

    *out = std::move(rf);
    return true;
}

bool SaveReplay(const ReplayFile& replay, const std::string& path, std::string* error, ReplayDetail detail)
{
    const std::vector<uint8_t> bytes = SerializeReplay(replay, detail);
    // Write to a temp name and rename so a crash never leaves half a report.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot open " + tmp;
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    std::remove(path.c_str());
    if (!written || std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool LoadReplay(const std::string& path, ReplayFile* out, std::string* error)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    return DeserializeReplay(bytes.data(), bytes.size(), out, error);
}

} // namespace sfv
