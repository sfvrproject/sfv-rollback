#include "core/mux.h"

#include <algorithm>

namespace sfv {

class Mux::View : public ITransport {
public:
    View(Mux& mux, uint8_t id) : mux_(mux), id_(id) {}

    void Send(uint64_t peer, const uint8_t* data, size_t len) override
    {
        std::vector<uint8_t> tagged(len + 1);
        tagged[0] = id_;
        std::copy(data, data + len, tagged.begin() + 1);
        std::lock_guard lock(mux_.mutex_);
        mux_.base_.Send(peer, tagged.data(), tagged.size());
    }

    bool Receive(Packet& out) override
    {
        std::lock_guard lock(mux_.mutex_);
        mux_.Pump();
        if (queue.empty()) return false;
        out = std::move(queue.front());
        queue.pop_front();
        return true;
    }

    std::deque<Packet> queue;

private:
    Mux& mux_;
    uint8_t id_;
};

Mux::Mux(ITransport& base) : base_(base) {}

Mux::~Mux() = default;

ITransport& Mux::Channel(uint8_t id)
{
    std::lock_guard lock(mutex_);
    auto& v = views_[id];
    if (!v) v = std::make_unique<View>(*this, id);
    return *v;
}

void Mux::Pump()
{
    Packet p;
    while (base_.Receive(p)) {
        if (p.data.empty()) continue;
        // Keep packets for channels not opened yet: the peer may be ahead.
        auto& view = views_[p.data[0]];
        if (!view) view = std::make_unique<View>(*this, p.data[0]);
        p.data.erase(p.data.begin());
        view->queue.push_back(std::move(p));
        if (view->queue.size() > 4096) view->queue.pop_front();
    }
}

void ConditionedTransport::SetConditions(const NetConditions& c)
{
    std::lock_guard lock(mutex_);
    conditions_ = c;
}

NetConditions ConditionedTransport::Conditions()
{
    std::lock_guard lock(mutex_);
    return conditions_;
}

void ConditionedTransport::Send(uint64_t peer, const uint8_t* data, size_t len)
{
    std::lock_guard lock(mutex_);
    const NetConditions c = conditions_;
    if (c.latencyMs == 0 && c.jitterMs == 0 && c.loss <= 0.0 && pending_.empty()) {
        base_.Send(peer, data, len);
        return;
    }
    if (c.loss > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < c.loss) return;
    int delay = c.latencyMs;
    if (c.jitterMs > 0) delay += static_cast<int>(rng_() % uint64_t(c.jitterMs + 1));
    pending_.push_back({Clock::now() + std::chrono::milliseconds(delay), seq_++, peer, {data, data + len}});
    Flush();
}

bool ConditionedTransport::Receive(Packet& out)
{
    {
        std::lock_guard lock(mutex_);
        Flush();
    }
    return base_.Receive(out);
}

void ConditionedTransport::Flush()
{
    const auto now = Clock::now();
    std::stable_sort(pending_.begin(), pending_.end(), [](const Pending& a, const Pending& b) { return a.due < b.due; });
    while (!pending_.empty() && pending_.front().due <= now) {
        base_.Send(pending_.front().peer, pending_.front().data.data(), pending_.front().data.size());
        pending_.pop_front();
    }
}

} // namespace sfv
