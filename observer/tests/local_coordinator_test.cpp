#include "shin/observer/local_coordinator.h"
#include "shin/observer/io.h"
#include <array>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace shin::observer;
using namespace std::chrono_literals;
using Clock = LocalCoordinator::Clock;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct NativeEvent {
#ifdef _WIN32
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    NativeEvent() { check(event != nullptr, "cannot create fixture event"); }
    ~NativeEvent() { CloseHandle(event); }
    LocalReadySource source() const { return {reinterpret_cast<std::intptr_t>(event)}; }
    void signal() { check(SetEvent(event) != 0, "cannot signal fixture event"); }
    bool signalled() const { return WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }
    void consume() { check(ResetEvent(event) != 0, "cannot reset fixture event"); }
#else
    std::array<int, 2> pipe{-1, -1};
    NativeEvent() {
        check(::pipe(pipe.data()) == 0, "cannot create fixture pipe");
        for (const auto fd : pipe)
            check(::fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
                  "cannot protect fixture pipe");
    }
    ~NativeEvent() { for (const auto fd : pipe) ::close(fd); }
    LocalReadySource source() const { return {pipe[0]}; }
    void signal() { const char byte = 1; check(::write(pipe[1], &byte, 1) == 1, "cannot signal fixture pipe"); }
    bool signalled() const { char byte = 0; return ::read(pipe[0], &byte, 1) == 1; }
    void consume() { char byte = 0; while (::read(pipe[0], &byte, 1) == 1) {} }
#endif
};

void timer_and_early_notifications() {
    NativeEvent stop;
    LocalCoordinator coordinator;
    coordinator.stop_on(stop.source());
    const auto before = Clock::now();
    coordinator.wait(before + 120ms);
    check(Clock::now() - before >= 100ms, "idle wait returned at the old 20 ms polling interval");
    const auto notify = coordinator.notifier();
    for (unsigned i = 0; i < 100; ++i) notify();
    const auto early = Clock::now();
    coordinator.wait(early + 2s);
    check(Clock::now() - early < 1s, "notification before wait was lost");
    std::jthread sender([&] { std::this_thread::sleep_for(50ms); notify(); });
    const auto waiting = Clock::now();
    coordinator.wait(waiting + 2s);
    check(Clock::now() - waiting < 1s, "notification during wait was lost");
}

void borrowed_native_readiness_and_reuse() {
    NativeEvent stop;
    LocalCoordinator coordinator;
    coordinator.stop_on(stop.source());
    for (unsigned i = 0; i < 12; ++i) {
        NativeEvent input;
        const std::array sources{input.source()};
        std::jthread sender([&] { std::this_thread::sleep_for(10ms); input.signal(); });
        const auto before = Clock::now();
        coordinator.wait(before + 2s, sources);
        check(Clock::now() - before < 1s && input.signalled(), "native readiness was missed or consumed by the coordinator");
        // The borrowed descriptor/event may close and be reused immediately.
    }
    std::vector<LocalReadySource> excessive(65);
    bool refused = false;
    try { coordinator.wait(Clock::now() + 1s, excessive); }
    catch (const Error& error) { refused = error.code == "service-event-loop-limit"; }
    check(refused, "unbounded readiness registration was accepted");
}

void notifier_survives_producer_teardown_race() {
    std::function<void()> notify;
    std::jthread sender;
    {
        LocalCoordinator coordinator;
        notify = coordinator.notifier();
        sender = std::jthread([notify](std::stop_token stop) {
            while (!stop.stop_requested()) { notify(); std::this_thread::yield(); }
        });
        coordinator.wait(Clock::now() + 1s);
    }
    for (unsigned i = 0; i < 100; ++i) notify();
    sender.request_stop(); sender.join();
}

#ifndef _WIN32
void signals_interrupt_idle_wait() {
    LocalCoordinator coordinator;
    coordinator.stop_on_signals();
    std::jthread sender([] { std::this_thread::sleep_for(50ms); ::kill(::getpid(), SIGTERM); });
    const auto before = Clock::now();
    coordinator.wait(before + 2s);
    check(coordinator.stopped() && Clock::now() - before < 1s, "shutdown signal did not wake the coordinator");
}
#endif
}

int main() {
    try {
        timer_and_early_notifications();
        borrowed_native_readiness_and_reuse();
        notifier_survives_producer_teardown_race();
#ifndef _WIN32
        signals_interrupt_idle_wait();
#endif
        std::cout << "coordinator timer, wakeup, borrowed IO, bounds and teardown passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
