#pragma once
// ITransport over a plain UDP socket, for two instances on one machine or a
// LAN. Peers are addressed as (IPv4 in host byte order << 16) | port.

#include "core/transport.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sfv::mod {

class UdpTransport : public ITransport {
public:
    // port 0 picks a free port.
    static std::unique_ptr<UdpTransport> Create(uint16_t port, std::string* error);
    ~UdpTransport() override;

    uint16_t Port() const { return port_; }

    void Send(uint64_t peer, const uint8_t* data, size_t len) override;
    bool Receive(Packet& out) override;

private:
    UdpTransport() = default;
    uintptr_t socket_ = ~uintptr_t(0);
    uint16_t port_ = 0;
};

// Dotted IPv4 text to host byte order; 0 on error.
uint32_t ParseIpv4(const std::string& text);

// How this machine reaches the internet: the adapter of the default route.
// 1 wired (Ethernet), 2 Wi-Fi, 0 unknown (lobby::Link).
uint8_t DetectLinkType();

} // namespace sfv::mod
