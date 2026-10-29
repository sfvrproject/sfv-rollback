#pragma once
// Transport plumbing for netplay:
//
//   Mux                 several channels over one transport (a leading byte
//                       per packet): the match handshake and GekkoNet share
//                       the connection.
//   ConditionedTransport adds latency, jitter (which reorders) and loss to the
//                       packets sent through it, in real time, for testing.

#include "core/transport.h"

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>

namespace sfv {

class Mux {
public:
    explicit Mux(ITransport& base);
    ~Mux();
    // A view of channel `id`: sends are tagged, receives see only that channel.
    ITransport& Channel(uint8_t id);

private:
    class View;
    void Pump();

    ITransport& base_;
    std::mutex mutex_;
    std::map<uint8_t, std::unique_ptr<View>> views_;
};

struct NetConditions {
    int latencyMs = 0; // one way, added to every packet this side sends
    int jitterMs = 0;  // extra 0..jitter ms, uniformly; reorders packets
    double loss = 0.0; // 0..1
    bool operator==(const NetConditions&) const = default;
};

class ConditionedTransport : public ITransport {
public:
    ConditionedTransport(ITransport& base, uint64_t seed) : base_(base), rng_(seed) {}
    void SetConditions(const NetConditions& c);
    NetConditions Conditions();

    void Send(uint64_t peer, const uint8_t* data, size_t len) override;
    bool Receive(Packet& out) override;

private:
    using Clock = std::chrono::steady_clock;
    struct Pending {
        Clock::time_point due;
        uint64_t seq;
        uint64_t peer;
        std::vector<uint8_t> data;
    };
    void Flush();

    ITransport& base_;
    std::mutex mutex_;
    std::mt19937_64 rng_;
    NetConditions conditions_;
    std::deque<Pending> pending_;
    uint64_t seq_ = 0;
};

} // namespace sfv
