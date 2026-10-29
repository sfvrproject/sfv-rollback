#include "core/replay_file.h"

#include "core/state.h"

#include <cstdio>
#include <cstring>

// Layout: "SFVR" u32 version, then chunks {u32 tag, u32 size, payload}, closed
// by an "END " chunk holding the CRC-32 of every byte before it. Unknown chunks
// are skipped so later versions can add data without breaking older tools.

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
constexpr uint32_t kHashes = Tag("HASH");
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
void WriteHashes(Writer& w, const ReplayFile& rf)
{
    const size_t regions = rf.RegionCount();
    const int frames = rf.FrameCount();
    w.U32(static_cast<uint32_t>(frames));
    w.U32(static_cast<uint32_t>(regions));

    std::vector<uint64_t> prev = rf.initialHashes;
    prev.resize(regions, 0);
    std::vector<uint8_t> bitmap((regions + 7) / 8);

    for (int f = 0; f < frames; f++) {
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
    std::vector<uint8_t> bitmap((regions + 7) / 8);
    rf.hashes.assign(size_t(frames) * regions, 0);

    for (uint32_t f = 0; f < frames && r.ok(); f++) {
        r.Raw(bitmap.data(), bitmap.size());
        for (uint32_t i = 0; i < regions; i++) {
            if (bitmap[i / 8] & (1u << (i % 8))) prev[i] = r.Get<uint64_t>();
        }
        std::memcpy(&rf.hashes[size_t(f) * regions], prev.data(), regions * sizeof(uint64_t));
    }
    return r.ok();
}

} // namespace

const uint64_t* ReplayFile::FrameHashes(int frame) const
{
    if (frame < 0) return initialHashes.data();
    return &hashes[size_t(frame) * RegionCount()];
}

uint32_t ReplayFile::FrameChecksum(int frame) const
{
    return FoldChecksum(FrameHashes(frame), RegionCount());
}

void ReplayFile::SetFrame(int frame, const FrameInputs& in, const uint64_t* regionHashes)
{
    if (frame < 0) return;
    const size_t regionCount = RegionCount();
    if (frame >= FrameCount()) {
        inputs.resize(size_t(frame) + 1);
        hashes.resize((size_t(frame) + 1) * regionCount, 0);
    }
    inputs[frame] = in;
    std::memcpy(&hashes[size_t(frame) * regionCount], regionHashes, regionCount * sizeof(uint64_t));
}

const StateDump* ReplayFile::FindDump(int frame) const
{
    for (const auto& d : dumps) {
        if (d.frame == frame) return &d;
    }
    return nullptr;
}

std::vector<uint8_t> SerializeReplay(const ReplayFile& rf)
{
    Writer w;
    w.U32(kMagic);
    w.U32(kReplayFormatVersion);

    size_t c = w.BeginChunk(kMeta);
    w.Str(rf.modVersion);
    w.Str(rf.gameBuild);
    w.U64(rf.matchId);
    w.I32(rf.localPlayer);
    w.U64(rf.createdUnix);
    w.U32(rf.inputDelay);
    w.U32(rf.predictionWindow);
    w.I32(rf.finalThrough);
    w.EndChunk(c);

    c = w.BeginChunk(kSetup);
    WriteSetup(w, rf.setup);
    w.EndChunk(c);

    c = w.BeginChunk(kRegions);
    w.U32(static_cast<uint32_t>(rf.regions.size()));
    for (const auto& r : rf.regions) {
        w.Str(r.name);
        w.U64(r.size);
        w.U32(static_cast<uint32_t>(r.ignore.size()));
        w.Raw(r.ignore.data(), r.ignore.size());
    }
    w.EndChunk(c);

    c = w.BeginChunk(kInitial);
    w.U32(static_cast<uint32_t>(rf.initialHashes.size()));
    for (uint64_t h : rf.initialHashes) w.U64(h);
    w.EndChunk(c);

    c = w.BeginChunk(kInputs);
    w.U32(static_cast<uint32_t>(rf.inputs.size()));
    for (const auto& in : rf.inputs) {
        for (PackedInput p : in.p) w.U32(p);
    }
    w.EndChunk(c);

    c = w.BeginChunk(kHashes);
    WriteHashes(w, rf);
    w.EndChunk(c);

    c = w.BeginChunk(kEvents);
    w.U32(static_cast<uint32_t>(rf.events.size()));
    for (const auto& e : rf.events) {
        w.I32(e.frame);
        w.U16(static_cast<uint16_t>(e.type));
        w.U16(0);
        w.U32(e.a);
        w.U32(e.b);
    }
    w.EndChunk(c);

    for (const auto& d : rf.dumps) {
        c = w.BeginChunk(kDump);
        w.I32(d.frame);
        w.Str(d.label);
        w.U32(static_cast<uint32_t>(d.bytes.size()));
        w.Raw(d.bytes.data(), d.bytes.size());
        w.EndChunk(c);
    }

    const uint32_t crc = Crc32(w.buf.data(), w.buf.size());
    c = w.BeginChunk(kEnd);
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
    if (version != kReplayFormatVersion) return fail("unsupported .sfvr format version");

    ReplayFile rf;
    bool sawEnd = false, sawInputs = false;

    while (r.ok() && r.left() > 0) {
        const size_t tagOffset = static_cast<size_t>(r.pos() - data);
        const uint32_t tag = r.Get<uint32_t>();
        const uint32_t size = r.Get<uint32_t>();
        if (!r.ok() || r.left() < size) return fail("truncated chunk");
        Reader c(r.pos(), size);
        r.Skip(size);

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
        case kHashes:
            // Needs the region table and the inputs, which are written first.
            if (!sawInputs) return fail("hash chunk before input chunk");
            if (!ReadHashes(c, rf, error)) return false;
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
        case kEnd:
            if (c.Get<uint32_t>() != Crc32(data, tagOffset)) return fail("checksum mismatch: file is corrupt");
            sawEnd = true;
            break;
        default:
            break; // newer chunk type
        }
        if (!c.ok()) return fail("malformed chunk");
        if (sawEnd) break;
    }

    if (!sawEnd) return fail("file is truncated (no END chunk)");
    if (rf.hashes.size() != size_t(rf.FrameCount()) * rf.RegionCount()) return fail("missing hash chunk");
    if (rf.initialHashes.size() != rf.RegionCount()) return fail("initial hashes do not match region table");

    *out = std::move(rf);
    return true;
}

bool SaveReplay(const ReplayFile& replay, const std::string& path, std::string* error)
{
    const std::vector<uint8_t> bytes = SerializeReplay(replay);
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
