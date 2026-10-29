#pragma once
// Match codes: what a host reads out to the player who joins.
//
//   XXXXXX               a Weyvelength room:
//                        the room code the server gave out (it runs with
//                        --room-code-length 6), 6 characters of
//                        ABCDEFGHJKLMNPQRSTUVWXYZ23456789; "W-" in front is
//                        allowed
//   U-XXXX-XXXX-XXXX-XX  UDP (testing): IPv4 address and port of the host,
//                        and a random nonce the host checks, so a stale or
//                        mistyped code cannot join someone else's match.
//                        Crockford base32 with a trailing check character
//
// Decoding ignores case, dashes and spaces; in UDP codes it reads O as 0 and
// I/L as 1.

#include <cstdint>
#include <string>

namespace sfv {

struct MatchCode {
    enum class Kind : uint8_t { Room = 1, Udp = 2 };
    Kind kind = Kind::Room;
    std::string room;     // Room: the 6 characters
    uint64_t address = 0; // UDP: (IPv4 address in host byte order << 16) | port
    uint16_t nonce = 0;   // UDP

    bool operator==(const MatchCode&) const = default;
};

std::string EncodeMatchCode(const MatchCode& code);
bool DecodeMatchCode(const std::string& text, MatchCode* out);

inline uint64_t UdpAddress(uint32_t ipv4HostOrder, uint16_t port) { return (uint64_t(ipv4HostOrder) << 16) | port; }

} // namespace sfv
