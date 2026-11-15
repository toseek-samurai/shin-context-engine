#include "shin/observer/local_coordinator.h"
#include "shin/observer/io.h"
#include <uv.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <mutex>
#include <optional>
#include <set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace shin::observer {
namespace {
void require_uv(int result) {
    if (result < 0) throw Error("service-event-loop-failed", "Native coordinator setup or wait failed.");
}
struct Wake {
    std::mutex gate;
    uv_async_t* async = nullptr;
    std::atomic<bool> pending{false};
    void notify() noexcept {
        std::lock_guard lock(gate);
        if (!async) return;
        pending.store(true, std::memory_order_release);
        (void)uv_async_send(async);
    }
};
}

class LocalCoordinator::Impl {
public:
    uv_loop_t loop{};
    uv_async_t async{};
    uv_timer_t timer{};
    uv_signal_t interrupt{}, terminate{};
    bool loop_open = false, async_open = false, timer_open = false;
    bool interrupt_open = false, terminate_open = false, observable_stop = false;
    bool ready = false, stop = false;
    std::optional<LocalReadySource> stop_source;
    Clock::time_point next_signal_poll{};
    std::shared_ptr<Wake> wake = std::make_shared<Wake>();

    void start() {
        require_uv(uv_loop_init(&loop)); loop_open = true;
        require_uv(uv_async_init(&loop, &async, [](uv_async_t*) {})); async_open = true;
        require_uv(uv_timer_init(&loop, &timer)); timer_open = true; timer.data = this;
        wake->async = &async;
    }
    void signals() {
        if (interrupt_open || terminate_open) return;
        const auto callback = [](uv_signal_t* signal, int) {
            auto& self = *static_cast<Impl*>(signal->data);
            self.stop = true; self.ready = true;
        };
        require_uv(uv_signal_init(&loop, &interrupt)); interrupt_open = true; interrupt.data = this;
        require_uv(uv_signal_start(&interrupt, callback, SIGINT));
        require_uv(uv_signal_init(&loop, &terminate)); terminate_open = true; terminate.data = this;
        require_uv(uv_signal_start(&terminate, callback, SIGTERM));
        observable_stop = true;
    }

    struct Registration {
        int error = 0;
#ifdef _WIN32
        HANDLE wait = nullptr;
        std::shared_ptr<Wake> wake;
        explicit Registration(Impl& owner, LocalReadySource source) : wake(owner.wake) {
            if (!RegisterWaitForSingleObject(&wait, reinterpret_cast<HANDLE>(source.native),
                    [](void* context, BOOLEAN) { static_cast<Registration*>(context)->wake->notify(); },
                    this, INFINITE, WT_EXECUTEONLYONCE))
                throw Error("service-event-loop-failed", "Cannot register a native operation event.");
        }
        ~Registration() { if (wait) (void)UnregisterWaitEx(wait, INVALID_HANDLE_VALUE); }
#else
        uv_poll_t poll{};
        bool open = false;
        Registration(Impl& owner, LocalReadySource source) {
            error = uv_poll_init(&owner.loop, &poll, static_cast<int>(source.native));
            if (error < 0) return;
            open = true;
            poll.data = &owner;
            error = uv_poll_start(&poll, source.writable ? UV_WRITABLE : UV_READABLE,
                [](uv_poll_t* handle, int, int) { static_cast<Impl*>(handle->data)->ready = true; });
        }
        void close() { if (open) { uv_poll_stop(&poll); uv_close(reinterpret_cast<uv_handle_t*>(&poll), nullptr); open = false; } }
#endif
    };

    void wait(Clock::time_point deadline, std::span<const LocalReadySource> input) {
        if (stop || wake->pending.exchange(false, std::memory_order_acq_rel)) return;
        if (!observable_stop) deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(20));
        if (deadline <= Clock::now()) return;
        if (input.size() > 64U - (stop_source ? 1U : 0U))
            throw Error("service-event-loop-limit", "Coordinator readiness bound exceeded.");
        std::vector<LocalReadySource> sources(input.begin(), input.end());
        if (stop_source) sources.push_back(*stop_source);
        if (std::any_of(sources.begin(), sources.end(), [](const auto& source) { return source.immediate; })) return;
        std::vector<std::unique_ptr<Registration>> registrations;
        registrations.reserve(sources.size());
        std::set<std::intptr_t> registered;
        const auto retire = [&] {
#ifndef _WIN32
            for (const auto& registration : registrations) registration->close();
            // Close callbacks run before their embedded handles are destroyed.
            uv_run(&loop, UV_RUN_NOWAIT);
#endif
            registrations.clear();
        };
        try {
            for (const auto source : sources) {
                if (source.native < 0 || !registered.insert(source.native).second) continue;
                registrations.push_back(std::make_unique<Registration>(*this, source));
                require_uv(registrations.back()->error);
            }
            ready = false;
            uv_update_time(&loop);
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            require_uv(uv_timer_start(&timer, [](uv_timer_t* handle) {
                static_cast<Impl*>(handle->data)->ready = true;
            }, static_cast<std::uint64_t>(std::max<std::int64_t>(0, left)), 0));
            while (!ready && !stop && !wake->pending.exchange(false, std::memory_order_acq_rel))
                uv_run(&loop, UV_RUN_ONCE);
            uv_timer_stop(&timer);
            retire();
        } catch (...) { uv_timer_stop(&timer); retire(); throw; }
    }

    ~Impl() {
        { std::lock_guard lock(wake->gate); wake->async = nullptr; }
        const auto close = [](auto& handle, bool open) {
            if (open) uv_close(reinterpret_cast<uv_handle_t*>(&handle), nullptr);
        };
        close(interrupt, interrupt_open); close(terminate, terminate_open);
        close(timer, timer_open); close(async, async_open);
        if (loop_open) { uv_run(&loop, UV_RUN_DEFAULT); (void)uv_loop_close(&loop); }
    }
};

LocalCoordinator::LocalCoordinator() : impl_(std::make_unique<Impl>()) { impl_->start(); }
LocalCoordinator::~LocalCoordinator() = default;
std::function<void()> LocalCoordinator::notifier() const {
    return [wake = impl_->wake] { wake->notify(); };
}
void LocalCoordinator::wait(Clock::time_point deadline, std::span<const LocalReadySource> sources) { impl_->wait(deadline, sources); }
void LocalCoordinator::stop_on_signals() { impl_->signals(); }
void LocalCoordinator::stop_on(LocalReadySource source) { impl_->stop_source = source; impl_->observable_stop = true; }
bool LocalCoordinator::stopped() const noexcept { return impl_->stop; }
void LocalCoordinator::poll_signals() {
    const auto now = Clock::now();
    if (!impl_->observable_stop || now < impl_->next_signal_poll) return;
    impl_->next_signal_poll = now + std::chrono::milliseconds(20);
    uv_run(&impl_->loop, UV_RUN_NOWAIT);
}
} // namespace shin::observer
