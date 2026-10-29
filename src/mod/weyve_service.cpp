#include "mod/weyve_service.h"

#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"

#include <weyvelength.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace sfv::mod {

namespace {

int64_t MicrosSince(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count();
}

constexpr uint16_t kDefaultPort = 5555;
constexpr size_t kMaxDatagram = 1024;
constexpr auto kLinkKick = std::chrono::milliseconds(200);
const char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

const char* RoomErrorName(WeyveRoomError e)
{
    switch (e) {
    case WEYVE_ROOM_ERROR_ALREADY_IN_ROOM: return "already in a room";
    case WEYVE_ROOM_ERROR_NO_SUCH_ROOM: return "no such room";
    case WEYVE_ROOM_ERROR_NOT_IN_ROOM: return "not in a room";
    case WEYVE_ROOM_ERROR_NOT_HOST: return "not the room's host";
    case WEYVE_ROOM_ERROR_BAD_ROOM_DATA: return "bad room data";
    case WEYVE_ROOM_ERROR_NO_SUCH_MEMBER: return "no such member";
    case WEYVE_ROOM_ERROR_ROOM_CLOSED: return "the room is closed";
    case WEYVE_ROOM_ERROR_BAD_PASSWORD: return "wrong password";
    case WEYVE_ROOM_ERROR_BANNED: return "banned from the room";
    case WEYVE_ROOM_ERROR_RATE_LIMITED: return "asked too often";
    }
    return "room error";
}

std::string Str(const char* p, uint32_t len) { return p ? std::string(p, len) : std::string(); }

// A getter's byte range (it writes the length through its last argument).
template <typename Get> std::string Bytes(Get get)
{
    uint32_t len = 0;
    const char* p = get(&len);
    return Str(p, len);
}

} // namespace

const WeyveMember* WeyveRoomView::Member(uint32_t id) const
{
    for (const WeyveMember& m : members) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

bool ParseServer(const std::string& text, std::string* host, uint16_t* port)
{
    *host = text;
    *port = kDefaultPort;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        *host = text.substr(0, colon);
        const int p = std::atoi(text.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        *port = static_cast<uint16_t>(p);
    }
    return !host->empty();
}

struct WeyveService::Impl {
    std::string host;
    uint16_t port = kDefaultPort;
    std::string name;

    std::mutex mutex; // guards everything below and every call into the client
    WeyveClient* client = nullptr;
    std::atomic<bool> connected{false};
    WeyveRoomView::Server server = WeyveRoomView::Server::Connecting;
    std::vector<std::function<void(WeyveClient*)>> commands;
    std::deque<WeyveEvent> events;
    std::deque<Packet> inbox;
    std::map<uint32_t, std::chrono::steady_clock::time_point> kicked; // per peer, last send while unlinked
    std::map<uint32_t, bool> linked;                                   // per peer, as last logged

    std::atomic<bool> stop{false};
    std::thread thread;

    void Queue(std::function<void(WeyveClient*)> command)
    {
        std::lock_guard lock(mutex);
        commands.push_back(std::move(command));
    }

    // With the lock held: the client's network work, then its events and
    // datagrams into our queues.
    void Pump()
    {
        if (!connected) return;
        // Polled on after the server connection is gone: the P2P links stay
        // (third_party/weyvelength-keep-links.patch).
        if (!weyve_poll(client) && server == WeyveRoomView::Server::Connected) {
            server = WeyveRoomView::Server::Lost;
            events.push_back({WeyveEvent::Type::ServerLost, 0, "lost the connection to " + host});
            Log("weyve: lost the connection to %s:%u; peer-to-peer links stay up", host.c_str(), port);
        }
        WeyveEvent ev{};
        ::WeyveEvent raw;
        while (weyve_next(client, &raw)) {
            switch (raw.type) {
            case WEYVE_EVENT_ROOM_ID_ASSIGNED:
                ev = {WeyveEvent::Type::RoomJoined, 0, Str(raw.data.room_assigned.id, raw.data.room_assigned.id_len)};
                Log("weyve: in room %s as member %u", ev.text.c_str(), weyve_id(client));
                events.push_back(ev);
                break;
            case WEYVE_EVENT_ROOM_ERROR:
                ev = {WeyveEvent::Type::RoomError, 0, RoomErrorName(raw.data.room_error.code)};
                Log("weyve: room error: %s %s", ev.text.c_str(),
                    Str(raw.data.room_error.context, raw.data.room_error.context_len).c_str());
                events.push_back(ev);
                break;
            case WEYVE_EVENT_PEER_JOINED:
                Log("weyve: member %u (%s) joined", raw.data.peer_joined.id,
                    Str(raw.data.peer_joined.name, raw.data.peer_joined.name_len).c_str());
                events.push_back({WeyveEvent::Type::PeerJoined, raw.data.peer_joined.id, {}});
                break;
            case WEYVE_EVENT_PEER_LEFT:
                if (raw.data.peer_left.id == weyve_id(client)) break; // our own leave
                Log("weyve: member %u left", raw.data.peer_left.id);
                kicked.erase(raw.data.peer_left.id);
                linked.erase(raw.data.peer_left.id);
                events.push_back({WeyveEvent::Type::PeerLeft, raw.data.peer_left.id, {}});
                break;
            case WEYVE_EVENT_KICKED:
            case WEYVE_EVENT_BANNED:
                Log("weyve: removed from the room by its host");
                events.push_back({WeyveEvent::Type::Removed, 0, "removed from the room"});
                break;
            default:
                break;
            }
        }
        uint32_t from = 0, len = 0;
        while (const uint8_t* data = weyve_next_p2p(client, &from, &len)) {
            Packet& p = inbox.emplace_back();
            p.from = from;
            p.data.assign(data, data + len);
        }
    }

    void Run()
    {
        ThreadGuard running;
        Log("weyve: connecting to %s:%u", host.c_str(), port);
        WeyveClient* c = weyve_client_create();
        if (!weyve_connect(c, host.c_str(), port)) {
            Log("weyve: cannot connect to %s:%u", host.c_str(), port);
            weyve_client_destroy(c);
            std::lock_guard lock(mutex);
            server = WeyveRoomView::Server::Failed;
            events.push_back({WeyveEvent::Type::ServerLost, 0, "cannot connect to " + host});
            return;
        }
        weyve_set_name(c, name.c_str());
        if (WeyveService::RelayOnly()) {
            weyve_set_relay_only(c, true);
            Log("weyve: relay only: links take only the peers' relay candidates");
        }
        {
            std::lock_guard lock(mutex);
            client = c;
            connected = true;
            server = WeyveRoomView::Server::Connected;
        }
        Log("weyve: connected to %s:%u", host.c_str(), port);

        // A high-resolution timer: a plain Sleep(1) waits a whole scheduler
        // tick (up to 15.6 ms) unless something raised the timer resolution.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        while (!stop && !Stopping()) {
            {
                const auto heldFrom = std::chrono::steady_clock::now();
                std::lock_guard lock(mutex);
                // The game thread's sends and receives wait while this holds
                // the lock: a long turn shows as a frame drop (hitch.h).
                struct Held {
                    std::chrono::steady_clock::time_point from;
                    ~Held()
                    {
                        static ULONGLONG lastLog = 0;
                        const int64_t us = MicrosSince(from);
                        if (us >= 8000 && GetTickCount64() - lastLog >= 1000) {
                            lastLog = GetTickCount64();
                            Log("hitch: the Weyvelength thread held the client lock %.1f ms", us / 1000.0);
                        }
                    }
                } held{heldFrom};
                if (connected && server == WeyveRoomView::Server::Connected) {
                    for (auto& command : commands) command(client);
                    commands.clear();
                }
                Pump();
                for (const auto& [peer, wasLinked] : std::map<uint32_t, bool>(linked)) {
                    const bool now = connected && weyve_peer_connected(client, peer);
                    if (now == wasLinked) continue;
                    linked[peer] = now;
                    Log("weyve: link to member %u %s", peer, now ? "up" : "down");
                }
            }
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -10000; // 1 ms
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 5);
            }
            else {
                Sleep(1);
            }
        }
        if (timer) CloseHandle(timer);

        std::lock_guard lock(mutex);
        if (connected && server == WeyveRoomView::Server::Connected) {
            // Leave, and give the request a moment to go out.
            weyve_leave_room(client);
            for (int i = 0; i < 5 && weyve_poll(client); i++) Sleep(2);
        }
        connected = false;
        weyve_client_destroy(client);
        client = nullptr;
    }
};

WeyveService::WeyveService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<WeyveService> WeyveService::Start(const std::string& server, const std::string& name)
{
    auto impl = std::make_unique<Impl>();
    if (!ParseServer(server, &impl->host, &impl->port)) impl->host = server;
    impl->name = name;
    Impl* raw = impl.get();
    std::unique_ptr<WeyveService> s(new WeyveService(std::move(impl)));
    raw->thread = std::thread([raw] { raw->Run(); });
    return s;
}

WeyveService::~WeyveService()
{
    impl_->stop = true;
    if (impl_->thread.joinable()) impl_->thread.join();
}

void WeyveService::CreateRoom()
{
    impl_->Queue([](WeyveClient* c) { weyve_create_room(c); });
}

void WeyveService::JoinRoom(const std::string& code)
{
    impl_->Queue([code](WeyveClient* c) { weyve_join_room(c, code.c_str(), nullptr); });
}

void WeyveService::LeaveRoom()
{
    impl_->Queue([](WeyveClient* c) { weyve_leave_room(c); });
}

void WeyveService::SetRoomData(const std::string& key, const std::string& value)
{
    impl_->Queue([key, value](WeyveClient* c) { weyve_set_room_data(c, key.c_str(), value.c_str()); });
}

void WeyveService::SetMemberData(const std::string& key, const std::string& value)
{
    impl_->Queue([key, value](WeyveClient* c) { weyve_set_member_data(c, key.c_str(), value.c_str()); });
}

WeyveRoomView WeyveService::View()
{
    std::lock_guard lock(impl_->mutex);
    WeyveRoomView v;
    v.server = impl_->server;
    v.serverText = impl_->host + ":" + std::to_string(impl_->port);
    if (!impl_->connected) return v;
    WeyveClient* c = impl_->client;
    v.self = weyve_id(c);
    uint32_t len = 0;
    v.room = Bytes([&](uint32_t* n) { return weyve_room_id(c, n); });
    if (v.room.empty()) return v;
    v.host = weyve_host_id(c);
    uint32_t count = 0;
    const uint32_t* ids = weyve_members(c, &count);
    for (uint32_t i = 0; i < count; i++) {
        WeyveMember& m = v.members.emplace_back();
        m.id = ids[i];
        m.name = Bytes([&](uint32_t* n) { return weyve_peer_name(c, m.id, n); });
        const uint32_t keys = weyve_member_data_count(c, m.id);
        for (uint32_t k = 0; k < keys; k++) {
            const std::string key = Bytes([&](uint32_t* n) { return weyve_member_data_key_at(c, m.id, k, n); });
            const char* value = weyve_member_data(c, m.id, key.c_str(), &len);
            if (value) m.data[key] = Str(value, len);
        }
    }
    const uint32_t keys = weyve_room_data_count(c);
    for (uint32_t k = 0; k < keys; k++) {
        const std::string key = Bytes([&](uint32_t* n) { return weyve_room_data_key_at(c, k, n); });
        const char* value = weyve_room_data(c, key.c_str(), &len);
        if (value) v.data[key] = Str(value, len);
    }
    return v;
}

bool WeyveService::NextEvent(WeyveEvent& out)
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->events.empty()) return false;
    out = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

bool WeyveService::PeerLinked(uint32_t id)
{
    std::lock_guard lock(impl_->mutex);
    return impl_->connected && weyve_peer_connected(impl_->client, id);
}

WeyveService::LinkPath WeyveService::PeerPath(uint32_t id)
{
    std::lock_guard lock(impl_->mutex);
    switch (weyve_peer_link_path(impl_->client, id)) {
    case WEYVE_LINK_DIRECT: return LinkPath::Direct;
    case WEYVE_LINK_RELAYED: return LinkPath::Relayed;
    case WEYVE_LINK_UNKNOWN: return LinkPath::Unknown;
    case WEYVE_LINK_NONE: break;
    }
    return LinkPath::None;
}

std::string WeyveService::TurnHost()
{
    std::lock_guard lock(impl_->mutex);
    const char* host = weyve_turn_host(impl_->client);
    return host ? host : "";
}

namespace {
std::atomic<bool> g_relayOnly{false};
}

void WeyveService::SetRelayOnly(bool on)
{
    g_relayOnly = on;
    Log("weyve: relay only %s (from the next session)", on ? "on" : "off");
}

bool WeyveService::RelayOnly() { return g_relayOnly; }

void WeyveService::Send(uint64_t peer, const uint8_t* data, size_t len)
{
    if (len == 0 || len > kMaxDatagram || peer > UINT32_MAX) return;
    const uint32_t id = static_cast<uint32_t>(peer);
    hitch::Scope timed(hitch::Part::Transport);
    const auto waitFrom = std::chrono::steady_clock::now();
    std::lock_guard lock(impl_->mutex);
    hitch::Add(hitch::Part::TransportWait, MicrosSince(waitFrom));
    if (!impl_->connected) return;
    if (!weyve_peer_connected(impl_->client, id)) {
        const auto now = std::chrono::steady_clock::now();
        auto [it, first] = impl_->kicked.try_emplace(id, now);
        if (!first && now - it->second < kLinkKick) return;
        it->second = now;
        impl_->linked.try_emplace(id, false);
    }
    weyve_send_p2p(impl_->client, id, data, static_cast<uint32_t>(len));
}

bool WeyveService::Receive(Packet& out)
{
    hitch::Scope timed(hitch::Part::Transport);
    const auto waitFrom = std::chrono::steady_clock::now();
    std::lock_guard lock(impl_->mutex);
    hitch::Add(hitch::Part::TransportWait, MicrosSince(waitFrom));
    if (impl_->inbox.empty()) impl_->Pump();
    if (impl_->inbox.empty()) return false;
    out = std::move(impl_->inbox.front());
    impl_->inbox.pop_front();
    return true;
}

bool WeyveService::NormaliseCode(const std::string& text, std::string* code)
{
    std::string s;
    for (char ch : text) {
        if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        s += static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    }
    if (s.size() == 7 && s[0] == 'W') s.erase(0, 1);
    if (s.size() != 6) return false;
    for (char ch : s) {
        if (!std::strchr(kCodeAlphabet, ch)) return false;
    }
    *code = s;
    return true;
}

std::string WeyveService::DisplayCode(const std::string& code)
{
    return code;
}

} // namespace sfv::mod
