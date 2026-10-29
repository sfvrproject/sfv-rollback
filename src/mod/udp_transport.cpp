#include "mod/udp_transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace sfv::mod {

std::unique_ptr<UdpTransport> UdpTransport::Create(uint16_t port, std::string* error)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        *error = "WSAStartup failed";
        return nullptr;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        *error = "socket failed";
        return nullptr;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // A peer that went away answers with ICMP port unreachable, which would
    // make recvfrom fail with WSAECONNRESET; ignore those.
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *error = "cannot bind UDP port " + std::to_string(port);
        closesocket(s);
        return nullptr;
    }
    int len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);

    std::unique_ptr<UdpTransport> t(new UdpTransport());
    t->socket_ = s;
    t->port_ = ntohs(addr.sin_port);
    return t;
}

UdpTransport::~UdpTransport()
{
    if (socket_ != ~uintptr_t(0)) closesocket(static_cast<SOCKET>(socket_));
}

void UdpTransport::Send(uint64_t peer, const uint8_t* data, size_t len)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(peer >> 16));
    addr.sin_port = htons(static_cast<uint16_t>(peer));
    sendto(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

bool UdpTransport::Receive(Packet& out)
{
    char buf[2048];
    sockaddr_in from{};
    int fromLen = sizeof(from);
    for (;;) {
        const int n = recvfrom(static_cast<SOCKET>(socket_), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                               &fromLen);
        if (n < 0) {
            if (WSAGetLastError() == WSAECONNRESET) continue;
            return false;
        }
        out.from = (uint64_t(ntohl(from.sin_addr.s_addr)) << 16) | ntohs(from.sin_port);
        out.data.assign(buf, buf + n);
        return true;
    }
}

uint32_t ParseIpv4(const std::string& text)
{
    in_addr a{};
    if (inet_pton(AF_INET, text.c_str(), &a) != 1) return 0;
    return ntohl(a.s_addr);
}

uint8_t DetectLinkType()
{
    // The interface a packet to a public address would leave through.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
    DWORD index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&to), &index) != NO_ERROR) return 0;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, list, &size) != NO_ERROR) {
            return 0;
        }
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->IfIndex != index) continue;
        if (a->IfType == IF_TYPE_IEEE80211) return 2;
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD) return 1;
        return 0;
    }
    return 0;
}

} // namespace sfv::mod
