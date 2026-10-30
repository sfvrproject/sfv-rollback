#include "core/lobby.h"

#include <algorithm>
#include <cstring>

namespace sfv::lobby {

namespace {

class Out {
public:
    std::vector<uint8_t> b;
    template <typename T> void Put(T v)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
};

class In {
public:
    In(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool ok = true;
    template <typename T> T Get()
    {
        T v{};
        if (size_t(end_ - p_) < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

void PutPlayer(Out& o, const PlayerSetup& p)
{
    o.Put(p.character);
    o.Put(p.costume);
    o.Put(p.color);
    o.Put(p.vskill);
    o.Put(p.vtrigger);
    o.Put(p.comment);
    o.Put(p.variation);
}

PlayerSetup GetPlayer(In& in)
{
    PlayerSetup p;
    p.character = in.Get<uint32_t>();
    p.costume = in.Get<uint16_t>();
    p.color = in.Get<uint8_t>();
    p.vskill = in.Get<uint8_t>();
    p.vtrigger = in.Get<uint8_t>();
    p.comment = in.Get<uint8_t>();
    p.variation = in.Get<uint8_t>();
    return p;
}

void PutSetup(Out& o, const MatchSetup& s)
{
    for (const auto& p : s.players) PutPlayer(o, p);
    o.Put(s.stage);
    o.Put(s.stageVariant);
    o.Put(s.roundsToWin);
    o.Put(s.timerSeconds);
    o.Put(s.seed);
    for (int i = 0; i < kNumPlayers; i++) o.Put(s.wins[i]);
    for (int i = 0; i < kNumPlayers; i++) o.Put(s.streaks[i]);
}

MatchSetup GetSetup(In& in)
{
    MatchSetup s;
    for (auto& p : s.players) p = GetPlayer(in);
    s.stage = in.Get<uint16_t>();
    s.stageVariant = in.Get<uint8_t>();
    s.roundsToWin = in.Get<uint8_t>();
    s.timerSeconds = in.Get<uint16_t>();
    s.seed = in.Get<uint64_t>();
    for (int i = 0; i < kNumPlayers; i++) s.wins[i] = in.Get<uint16_t>();
    for (int i = 0; i < kNumPlayers; i++) s.streaks[i] = in.Get<uint16_t>();
    return s;
}

void PutName(Out& o, const std::string& name)
{
    // Cut between characters, not inside a UTF-8 sequence.
    size_t n = std::min(name.size(), kMaxName);
    while (n > 0 && n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) n--;
    o.Put(static_cast<uint8_t>(n));
    o.b.insert(o.b.end(), name.begin(), name.begin() + n);
}

std::string GetName(In& in)
{
    const uint8_t n = in.Get<uint8_t>();
    if (n > kMaxName) in.ok = false;
    std::string s;
    for (uint8_t i = 0; i < n && in.ok; i++) s += static_cast<char>(in.Get<uint8_t>());
    return s;
}

void PutSeat(Out& o, const Seat& s, bool withName)
{
    if (withName) PutName(o, s.name);
    o.Put(static_cast<uint8_t>(int(s.present) | int(s.hasPick) << 1 | int(s.ready) << 2 | int(s.preloaded) << 3));
    PutPlayer(o, s.pick);
    o.Put(static_cast<uint8_t>(s.choice));
    o.Put(s.wins);
    o.Put(s.inputDelay);
    o.Put(s.fps);
    o.Put(static_cast<uint8_t>(s.link));
}

Seat GetSeat(In& in, bool withName)
{
    Seat s;
    if (withName) s.name = GetName(in);
    const uint8_t flags = in.Get<uint8_t>();
    s.present = flags & 1;
    s.hasPick = flags & 2;
    s.ready = flags & 4;
    s.preloaded = flags & 8;
    s.pick = GetPlayer(in);
    s.choice = static_cast<Choice>(in.Get<uint8_t>());
    s.wins = in.Get<uint16_t>();
    s.inputDelay = in.Get<uint8_t>();
    s.fps = in.Get<uint8_t>();
    s.link = static_cast<Link>(in.Get<uint8_t>());
    return s;
}

void PutClock(Out& o, const Message& m)
{
    o.Put(m.time);
    o.Put(m.echo);
    o.Put(m.echoAge);
}

void GetClock(In& in, Message& m)
{
    m.time = in.Get<uint32_t>();
    m.echo = in.Get<uint32_t>();
    m.echoAge = in.Get<uint16_t>();
}

} // namespace

std::vector<uint8_t> Encode(const Message& m)
{
    Out o;
    o.Put(kMagic);
    o.Put(m.protocol);
    o.Put(static_cast<uint8_t>(m.type));
    switch (m.type) {
    case Type::Hello:
        o.Put(m.nonce);
        o.Put(m.build);
        o.Put(static_cast<uint8_t>(m.role));
        PutName(o, m.name);
        break;
    case Type::Accept:
        o.Put(static_cast<uint8_t>(m.role));
        PutName(o, m.name);
        break;
    case Type::PlayerState:
        PutClock(o, m);
        PutSeat(o, m.seat, false);
        break;
    case Type::SessionState:
        PutClock(o, m);
        for (const Seat& seat : m.seats) PutSeat(o, seat, true);
        o.Put(static_cast<uint8_t>(m.phase));
        o.Put(m.played);
        o.Put(m.spectators);
        o.Put(m.stage);
        o.Put(m.matchId);
        break;
    case Type::Inputs:
        o.Put(m.matchId);
        o.Put(m.first);
        o.Put(static_cast<uint16_t>(m.frames.size()));
        for (const FrameRecord& f : m.frames) {
            for (PackedInput p : f.inputs.p) o.Put(p);
            o.Put(f.checksum);
        }
        break;
    case Type::InputsAck:
        o.Put(m.matchId);
        o.Put(m.first);
        break;
    case Type::Welcome:
        o.Put(m.matchId);
        PutSetup(o, m.setup);
        o.Put(m.inputDelay);
        o.Put(m.predictionWindow);
        break;
    case Type::Reject:
        o.Put(static_cast<uint8_t>(m.reason));
        break;
    case Type::Loaded:
        o.Put(m.matchId);
        PutSetup(o, m.setup);
        o.Put(m.stateChecksum);
        o.Put(static_cast<uint8_t>(m.reconciled));
        break;
    case Type::Bye:
        break;
    case Type::StateHashes:
        o.Put(m.matchId);
        o.Put(m.first);
        o.Put(m.total);
        o.Put(static_cast<uint16_t>(m.hashes.size()));
        for (uint64_t h : m.hashes) o.Put(h);
        break;
    case Type::StateRequest:
        o.Put(m.matchId);
        o.Put(static_cast<uint16_t>(m.regions.size()));
        for (uint32_t r : m.regions) o.Put(r);
        break;
    case Type::StateData:
        o.Put(m.matchId);
        o.Put(m.region);
        o.Put(m.total);
        o.Put(m.offset);
        o.Put(static_cast<uint16_t>(m.bytes.size()));
        o.b.insert(o.b.end(), m.bytes.begin(), m.bytes.end());
        break;
    }
    return std::move(o.b);
}

std::optional<Message> Decode(const uint8_t* data, size_t len)
{
    In in(data, len);
    if (in.Get<uint32_t>() != kMagic) return std::nullopt;
    Message m;
    m.protocol = in.Get<uint16_t>();
    m.type = static_cast<Type>(in.Get<uint8_t>());
    if (!in.ok) return std::nullopt;
    if (m.protocol != kProtocol) {
        // Still a message; the receiver answers ProtocolMismatch.
        return m;
    }
    switch (m.type) {
    case Type::Hello:
        m.nonce = in.Get<uint16_t>();
        m.build = in.Get<uint64_t>();
        m.role = static_cast<Role>(in.Get<uint8_t>());
        m.name = GetName(in);
        break;
    case Type::Accept:
        m.role = static_cast<Role>(in.Get<uint8_t>());
        m.name = GetName(in);
        break;
    case Type::PlayerState:
        GetClock(in, m);
        m.seat = GetSeat(in, false);
        break;
    case Type::SessionState:
        GetClock(in, m);
        for (Seat& seat : m.seats) seat = GetSeat(in, true);
        m.phase = static_cast<SessionPhase>(in.Get<uint8_t>());
        m.played = in.Get<uint16_t>();
        m.spectators = in.Get<uint8_t>();
        m.stage = in.Get<uint16_t>();
        m.matchId = in.Get<uint64_t>();
        break;
    case Type::Inputs: {
        m.matchId = in.Get<uint64_t>();
        m.first = in.Get<uint32_t>();
        const uint16_t n = in.Get<uint16_t>();
        if (n > kInputsPerMessage) return std::nullopt;
        for (uint16_t i = 0; i < n && in.ok; i++) {
            FrameRecord& f = m.frames.emplace_back();
            for (PackedInput& p : f.inputs.p) p = in.Get<PackedInput>();
            f.checksum = in.Get<uint32_t>();
        }
        break;
    }
    case Type::InputsAck:
        m.matchId = in.Get<uint64_t>();
        m.first = in.Get<uint32_t>();
        break;
    case Type::Welcome:
        m.matchId = in.Get<uint64_t>();
        m.setup = GetSetup(in);
        m.inputDelay = in.Get<uint8_t>();
        m.predictionWindow = in.Get<uint8_t>();
        break;
    case Type::Reject:
        m.reason = static_cast<RejectReason>(in.Get<uint8_t>());
        break;
    case Type::Loaded:
        m.matchId = in.Get<uint64_t>();
        m.setup = GetSetup(in);
        m.stateChecksum = in.Get<uint32_t>();
        m.reconciled = in.Get<uint8_t>() != 0;
        break;
    case Type::Bye:
        break;
    case Type::StateHashes: {
        m.matchId = in.Get<uint64_t>();
        m.first = in.Get<uint32_t>();
        m.total = in.Get<uint32_t>();
        const uint16_t n = in.Get<uint16_t>();
        if (n > kHashesPerMessage) return std::nullopt;
        for (uint16_t i = 0; i < n && in.ok; i++) m.hashes.push_back(in.Get<uint64_t>());
        break;
    }
    case Type::StateRequest: {
        m.matchId = in.Get<uint64_t>();
        const uint16_t n = in.Get<uint16_t>();
        if (n > kRegionsPerRequest) return std::nullopt;
        for (uint16_t i = 0; i < n && in.ok; i++) m.regions.push_back(in.Get<uint32_t>());
        break;
    }
    case Type::StateData: {
        m.matchId = in.Get<uint64_t>();
        m.region = in.Get<uint32_t>();
        m.total = in.Get<uint32_t>();
        m.offset = in.Get<uint32_t>();
        const uint16_t n = in.Get<uint16_t>();
        if (n > kStateChunk) return std::nullopt;
        for (uint16_t i = 0; i < n && in.ok; i++) m.bytes.push_back(in.Get<uint8_t>());
        break;
    }
    default:
        return std::nullopt;
    }
    if (!in.ok) return std::nullopt;
    return m;
}

} // namespace sfv::lobby
