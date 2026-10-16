#include "store_deadlines.h"

#include <array>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using shin::server::StoreDeadlines;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct SocketPair {
    std::array<int, 2> descriptors{-1, -1};
    SocketPair() { check(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0, "socketpair failed"); }
    ~SocketPair() { for (int fd : descriptors) if (fd >= 0) ::close(fd); }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    int socket() const { return descriptors[0]; }
    int peer() const { return descriptors[1]; }
};

bool readable(int descriptor, int milliseconds) {
    pollfd watched{descriptor, POLLIN, 0};
    const auto result = ::poll(&watched, 1, milliseconds);
    check(result >= 0, "poll failed");
    return result != 0;
}

void roundtrip(const SocketPair& pair) {
    char value = 'x';
    check(::send(pair.socket(), &value, 1, 0) == 1, "live socket write failed");
    check(readable(pair.peer(), 1000), "live socket read stalled");
    check(::recv(pair.peer(), &value, 1, 0) == 1 && value == 'x', "live socket data changed");
}

void expires_only_armed_slot() {
    SocketPair expired, untouched;
    StoreDeadlines monitor(2);
    monitor.arm(0, expired.socket(), 50ms);
    monitor.arm(1, untouched.socket(), 10s);
    check(readable(expired.peer(), 3000), "deadline did not interrupt the socket");
    char value;
    check(::recv(expired.peer(), &value, 1, 0) == 0, "deadline did not close I/O");
    roundtrip(untouched);
    check(monitor.disarm(0), "expired connection would remain pooled");
    check(!monitor.disarm(1), "unexpired connection was marked failed");
}

void disarm_and_rearm_cancel_previous_deadline() {
    SocketPair previous, current;
    StoreDeadlines monitor(1);
    monitor.arm(0, previous.socket(), 100ms);
    check(!monitor.disarm(0), "cancelled deadline was marked expired");
    monitor.arm(0, current.socket(), 300ms);
    check(!readable(previous.peer(), 150), "cancelled deadline interrupted old socket");
    roundtrip(previous);
    check(!readable(current.peer(), 0), "old deadline interrupted new lease");
    check(readable(current.peer(), 3000), "rearmed deadline did not run");
    check(monitor.disarm(0), "rearmed expiry was not reported");
}

void driver_descriptor_reuse_cannot_interrupt_another_socket() {
    SocketPair original;
    StoreDeadlines monitor(1);
    const int reused = original.socket();
    monitor.arm(0, reused, 100ms);
    ::close(original.descriptors[0]);
    original.descriptors[0] = -1;
    SocketPair unrelated;
    check(unrelated.socket() == reused, "fixture did not reuse the driver's descriptor");
    check(readable(original.peer(), 3000), "owned duplicate did not expire");
    roundtrip(unrelated);
    check(monitor.disarm(0), "closed driver descriptor hid expiry");
}

void concurrent_slots_remain_independent() {
    StoreDeadlines monitor(8);
    std::vector<std::jthread> callers;
    callers.reserve(8);
    for (std::size_t slot = 0; slot < 8; ++slot) {
        callers.emplace_back([&, slot] {
            SocketPair pair;
            for (int round = 0; round < 200; ++round) {
                monitor.arm(slot, pair.socket(), 10s);
                monitor.disarm(slot);
            }
            roundtrip(pair);
        });
    }
    for (auto& caller : callers) caller.join();
}

void destruction_disarms_without_waiting_or_interrupting() {
    SocketPair pair;
    const auto start = std::chrono::steady_clock::now();
    {
        StoreDeadlines monitor(1);
        monitor.arm(0, pair.socket(), 60s);
    }
    check(std::chrono::steady_clock::now() - start < 1s, "monitor shutdown waited for a lease timeout");
    roundtrip(pair);
}

void failed_duplicate_leaves_slot_reusable() {
    StoreDeadlines monitor(1);
    bool rejected = false;
    try { monitor.arm(0, -1, 1s); }
    catch (const std::system_error&) { rejected = true; }
    check(rejected, "invalid driver descriptor was accepted");
    check(!monitor.disarm(0), "failed arm was marked expired");
    SocketPair pair;
    monitor.arm(0, pair.socket(), 50ms);
    check(readable(pair.peer(), 3000), "failed arm retained or disabled the slot");
    monitor.disarm(0);
}
} // namespace

int main() {
    try {
        expires_only_armed_slot();
        disarm_and_rearm_cancel_previous_deadline();
        driver_descriptor_reuse_cannot_interrupt_another_socket();
        concurrent_slots_remain_independent();
        destruction_disarms_without_waiting_or_interrupting();
        failed_duplicate_leaves_slot_reusable();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "store deadline checks passed (6)\n";
}
