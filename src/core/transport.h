#pragma once
// Unreliable datagram transport between peers identified by a 64-bit id
// (a SteamID64 in the game, an arbitrary id in tests).

#include <cstdint>
#include <vector>

namespace sfv {

struct Packet {
    uint64_t from = 0;
    std::vector<uint8_t> data;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void Send(uint64_t peer, const uint8_t* data, size_t len) = 0;
    // Pops the next received packet; false when none is waiting.
    virtual bool Receive(Packet& out) = 0;
};

} // namespace sfv
