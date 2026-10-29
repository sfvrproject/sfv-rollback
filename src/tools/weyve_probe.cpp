// Checks a Weyvelength server and the peer-to-peer path without the game.
//
//   weyve_probe host [server[:port]]          creates a room, prints its code,
//                                             answers pings
//   weyve_probe join <code> [server[:port]]   joins, pings the host 20 times
//                                             over P2P, prints the round trips
//
// Server defaults to 127.0.0.1:5555.

#include <weyvelength.h>

#include <windows.h>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

bool Connect(WeyveClient* c, const std::string& server)
{
    std::string host = server;
    uint16_t port = 5555;
    if (const size_t colon = server.rfind(':'); colon != std::string::npos) {
        host = server.substr(0, colon);
        port = static_cast<uint16_t>(std::stoi(server.substr(colon + 1)));
    }
    if (!weyve_connect(c, host.c_str(), port)) {
        std::printf("cannot connect to %s:%u\n", host.c_str(), port);
        return false;
    }
    return true;
}

// Polls until an event of `type` arrives (true) or `ms` pass.
bool WaitFor(WeyveClient* c, WeyveEventType type, int ms, WeyveEvent* out)
{
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (!weyve_poll(c)) return false;
        WeyveEvent ev;
        while (weyve_next(c, &ev)) {
            if (ev.type == WEYVE_EVENT_ROOM_ERROR) std::printf("room error %d\n", int(ev.data.room_error.code));
            if (ev.type == type) {
                *out = ev;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: weyve_probe host [server] | join <code> [server]\n");
        return 2;
    }
    timeBeginPeriod(1); // 1 ms sleeps, as the mod's network thread gets
    const std::string mode = argv[1];
    const bool host = mode == "host";
    const std::string server = argc > (host ? 2 : 3) ? argv[host ? 2 : 3] : "127.0.0.1:5555";
    WeyveClient* c = weyve_client_create();
    if (!Connect(c, server)) return 1;
    weyve_set_name(c, host ? "probe-host" : "probe-join");

    WeyveEvent ev;
    if (host) weyve_create_room(c);
    else weyve_join_room(c, argv[2], nullptr);
    if (!WaitFor(c, WEYVE_EVENT_ROOM_ID_ASSIGNED, 5000, &ev)) {
        std::printf("no room\n");
        return 1;
    }
    std::printf("room %.*s, my id %u\n", int(ev.data.room_assigned.id_len), ev.data.room_assigned.id, weyve_id(c));
    std::fflush(stdout);

    if (host) {
        // Echo every datagram back to its sender, until the room empties.
        for (;;) {
            if (!weyve_poll(c)) return 1;
            while (weyve_next(c, &ev)) {
                if (ev.type == WEYVE_EVENT_PEER_JOINED) std::printf("joined: %u\n", ev.data.peer_joined.id);
                if (ev.type == WEYVE_EVENT_PEER_LEFT) {
                    std::printf("left: %u\n", ev.data.peer_left.id);
                    return 0;
                }
                std::fflush(stdout);
            }
            uint32_t from, len;
            while (const uint8_t* data = weyve_next_p2p(c, &from, &len)) {
                std::vector<uint8_t> copy(data, data + len);
                weyve_send_p2p(c, from, copy.data(), len);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    const uint32_t hostId = weyve_host_id(c);
    const auto start = Clock::now();
    std::vector<double> rtts;
    for (int i = 0; i < 20; i++) {
        const auto sent = Clock::now();
        uint8_t ping[64] = {};
        std::memcpy(ping, &i, sizeof(i));
        weyve_send_p2p(c, hostId, ping, sizeof(ping));
        bool got = false;
        while (!got && Clock::now() - sent < std::chrono::seconds(10)) {
            if (!weyve_poll(c)) return 1;
            while (weyve_next(c, &ev)) {}
            uint32_t from, len;
            while (const uint8_t* data = weyve_next_p2p(c, &from, &len)) {
                int n;
                std::memcpy(&n, data, sizeof(n));
                if (n == i) got = true;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (!got) {
            std::printf("ping %d: no answer\n", i);
            return 1;
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - sent).count();
        if (i == 0) {
            std::printf("link up after %.0f ms\n",
                        std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        else {
            rtts.push_back(ms);
        }
    }
    double sum = 0, worst = 0;
    for (double r : rtts) {
        sum += r;
        if (r > worst) worst = r;
    }
    std::printf("rtt avg %.2f ms, worst %.2f ms over %zu pings\n", sum / rtts.size(), worst, rtts.size());
    weyve_leave_room(c);
    WaitFor(c, WEYVE_EVENT_PEER_LEFT, 1000, &ev);
    weyve_client_destroy(c);
    return 0;
}
