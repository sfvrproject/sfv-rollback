#include "core/gekko_bridge.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace sfv {

namespace {

thread_local ITransport* t_transport = nullptr;
thread_local std::vector<GekkoNetResult*> t_results;

void SendData(GekkoNetAddress* addr, const char* data, int length)
{
    if (!t_transport || !addr || addr->size != sizeof(uint64_t) || length <= 0) return;
    uint64_t peer;
    std::memcpy(&peer, addr->data, sizeof(peer));
    t_transport->Send(peer, reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(length));
}

// GekkoNet frees each result, its address and its data through FreeData.
GekkoNetResult** ReceiveData(int* length)
{
    t_results.clear();
    Packet pkt;
    while (t_transport && t_transport->Receive(pkt)) {
        auto* res = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
        auto* addr = std::malloc(sizeof(uint64_t));
        auto* data = std::malloc(pkt.data.empty() ? 1 : pkt.data.size());
        if (!res || !addr || !data) {
            std::free(res);
            std::free(addr);
            std::free(data);
            break;
        }
        std::memcpy(addr, &pkt.from, sizeof(uint64_t));
        std::memcpy(data, pkt.data.data(), pkt.data.size());
        res->addr.data = addr;
        res->addr.size = sizeof(uint64_t);
        res->data = data;
        res->data_len = static_cast<unsigned int>(pkt.data.size());
        t_results.push_back(res);
    }
    *length = static_cast<int>(t_results.size());
    return t_results.data();
}

void FreeData(void* ptr) { std::free(ptr); }

GekkoNetAdapter g_adapter{SendData, ReceiveData, FreeData};

} // namespace

GekkoNetAdapter* GekkoAdapter() { return &g_adapter; }

GekkoTransportScope::GekkoTransportScope(ITransport* transport) : previous_(t_transport) { t_transport = transport; }

GekkoTransportScope::~GekkoTransportScope() { t_transport = previous_; }

} // namespace sfv
