#pragma once
// A Weyvelength client: one connection to the
// rooms server, one room at a time, peer-to-peer datagrams to the room's
// members over ICE links.
//
// The client is single-threaded; this service owns it on a network thread of
// its own that connects, polls about every millisecond and runs the room
// requests. Sends and receives (ITransport, peers are member ids) take the
// client's lock briefly from any thread; a receive polls first, so datagrams
// do not wait for the network thread's next turn.

#include "core/transport.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sfv::mod {

struct WeyveMember {
    uint32_t id = 0;
    std::string name;
    std::map<std::string, std::string> data;
};

// The room as last polled: a copy, consistent in itself.
struct WeyveRoomView {
    enum class Server { Connecting, Connected, Failed, Lost };
    Server server = Server::Connecting;
    std::string serverText; // "host:port"
    uint32_t self = 0;
    std::string room;       // the code; empty when not in a room
    uint32_t host = 0;
    std::vector<WeyveMember> members; // ourselves included
    std::map<std::string, std::string> data;
    const WeyveMember* Member(uint32_t id) const;
};

// Room happenings, in order, for the session driver.
struct WeyveEvent {
    enum class Type { RoomJoined, RoomError, PeerJoined, PeerLeft, Removed, ServerLost };
    Type type = Type::RoomJoined;
    uint32_t peer = 0;
    std::string text; // the room code (RoomJoined), what failed (RoomError, ServerLost)
};

class WeyveService : public ITransport {
public:
    // Starts connecting to `server` ("host[:port]", port 5555 by default) as
    // `name`. Never fails here: a failed connection shows in View().
    // `alsoPort`: the same host on this port too, at the same time (some
    // networks let only common ports through); the first to connect is used.
    static std::unique_ptr<WeyveService> Start(const std::string& server, const std::string& name,
                                               uint16_t alsoPort = 0);
    ~WeyveService() override; // leaves the room and disconnects

    // Run on the network thread once connected (queued until then).
    void CreateRoom();
    void JoinRoom(const std::string& code);
    void LeaveRoom();
    void SetRoomData(const std::string& key, const std::string& value);
    void SetMemberData(const std::string& key, const std::string& value);

    WeyveRoomView View();
    bool NextEvent(WeyveEvent& out);
    bool PeerLinked(uint32_t id); // a direct P2P link is up
    // How a member's link travels, from its selected ICE candidate pair
    // (third_party/weyvelength.patch): peer to peer, or relayed through TURN.
    enum class LinkPath { None, Unknown, Direct, Relayed };
    LinkPath PeerPath(uint32_t id);
    // The first TURN server the rooms server handed out ("turn.cloudflare.com");
    // empty without one.
    std::string TurnHost();
    // Testing (the panel's Debug tab): clients started from now on take only
    // their peers' relay candidates, so links go through TURN or not at all.
    static void SetRelayOnly(bool on);
    static bool RelayOnly();

    // ITransport. Datagrams over 1024 bytes are dropped (Weyvelength's limit).
    // While a peer's link is not up, sends are kept to one per 200 ms (enough
    // to start building the link): the client would queue every one.
    void Send(uint64_t peer, const uint8_t* data, size_t len) override;
    bool Receive(Packet& out) override;

    // Room codes: 6 characters of ABCDEFGHJKLMNPQRSTUVWXYZ23456789. Takes
    // lower case, dashes, spaces and a leading "W-"; false if it is no code.
    static bool NormaliseCode(const std::string& text, std::string* code);
    // As shown to players: "ABCDEF".
    static std::string DisplayCode(const std::string& code);

private:
    struct Impl;
    explicit WeyveService(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// "host:port" from [weyve] server, with the port defaulted.
bool ParseServer(const std::string& text, std::string* host, uint16_t* port);

} // namespace sfv::mod
