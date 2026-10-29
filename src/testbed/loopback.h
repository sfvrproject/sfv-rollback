#pragma once
// In-memory network for tests: latency, jitter (which reorders packets) and
// loss, driven by a virtual clock advanced once per simulated rendered frame.

#include "core/transport.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>

namespace sfv {

struct LinkConditions {
    int latencyFrames = 0;
    int jitterFrames = 0;
    double loss = 0.0;
};

class LoopbackNetwork {
public:
    LoopbackNetwork(uint64_t seed, LinkConditions link);
    ~LoopbackNetwork();

    ITransport& Endpoint(uint64_t id);
    void Tick();
    void SetLink(LinkConditions link) { link_ = link; }
    // Drops everything sent to or from `id` from now on.
    void Cut(uint64_t id) { cut_ = id; }

private:
    class Endpoint_;
    struct InFlight {
        uint64_t deliverAt;
        uint64_t seq;
        uint64_t from, to;
        std::vector<uint8_t> data;
    };

    void Send(uint64_t from, uint64_t to, const uint8_t* data, size_t len);

    LinkConditions link_;
    std::mt19937_64 rng_;
    uint64_t now_ = 0;
    uint64_t seq_ = 0;
    uint64_t cut_ = 0;
    std::deque<InFlight> inFlight_;
    std::map<uint64_t, std::unique_ptr<Endpoint_>> endpoints_;
};

} // namespace sfv
