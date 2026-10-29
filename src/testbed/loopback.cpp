#include "testbed/loopback.h"

#include <algorithm>

namespace sfv {

class LoopbackNetwork::Endpoint_ : public ITransport {
public:
    Endpoint_(LoopbackNetwork& net, uint64_t id) : net_(net), id_(id) {}

    void Send(uint64_t peer, const uint8_t* data, size_t len) override { net_.Send(id_, peer, data, len); }

    bool Receive(Packet& out) override
    {
        if (inbox.empty()) return false;
        out = std::move(inbox.front());
        inbox.pop_front();
        return true;
    }

    std::deque<Packet> inbox;

private:
    LoopbackNetwork& net_;
    uint64_t id_;
};

LoopbackNetwork::LoopbackNetwork(uint64_t seed, LinkConditions link) : link_(link), rng_(seed) {}

LoopbackNetwork::~LoopbackNetwork() = default;

ITransport& LoopbackNetwork::Endpoint(uint64_t id)
{
    auto& ep = endpoints_[id];
    if (!ep) ep = std::make_unique<Endpoint_>(*this, id);
    return *ep;
}

void LoopbackNetwork::Send(uint64_t from, uint64_t to, const uint8_t* data, size_t len)
{
    if (cut_ && (from == cut_ || to == cut_)) return;
    if (std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < link_.loss) return;
    uint64_t delay = static_cast<uint64_t>(link_.latencyFrames);
    if (link_.jitterFrames > 0) delay += rng_() % static_cast<uint64_t>(link_.jitterFrames + 1);
    inFlight_.push_back({now_ + delay, seq_++, from, to, std::vector<uint8_t>(data, data + len)});
}

void LoopbackNetwork::Tick()
{
    now_++;
    // Deliver in arrival order; jitter makes that differ from send order.
    std::stable_sort(inFlight_.begin(), inFlight_.end(),
                     [](const InFlight& a, const InFlight& b) { return a.deliverAt < b.deliverAt; });
    while (!inFlight_.empty() && inFlight_.front().deliverAt <= now_) {
        InFlight& p = inFlight_.front();
        auto it = endpoints_.find(p.to);
        if (it != endpoints_.end()) it->second->inbox.push_back({p.from, std::move(p.data)});
        inFlight_.pop_front();
    }
}

} // namespace sfv
