#include "shin/server/knowledge_scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace shin::server;
using namespace std::chrono_literals;

constexpr auto kWaitTimeout = 3s;
constexpr std::string_view kProject = "project-one";
constexpr std::string_view kRoute = "route-one";

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

KnowledgeConfig policy(bool enabled, int interval_seconds = 60, int max_age_seconds = 180) {
    KnowledgeConfig result;
    result.enabled = enabled;
    result.interval_seconds = interval_seconds;
    result.max_age_seconds = max_age_seconds;
    return result;
}

std::vector<KnowledgeTarget> targets() {
    return {{std::string(kProject), std::string(kRoute)}};
}

Json healthy_report(std::string generation) {
    return {{"healthy", true}, {"assessment_complete", true}, {"generation", std::move(generation)},
            {"reason_codes", Json::array()}, {"mutations_performed", false}};
}

class CallCounter {
public:
    int enter() {
        std::lock_guard lock(mutex_);
        ++calls_;
        wake_.notify_all();
        return calls_;
    }

    bool wait_for_at_least(int expected) {
        std::unique_lock lock(mutex_);
        return wake_.wait_for(lock, kWaitTimeout, [&] { return calls_ >= expected; });
    }

    int value() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    int calls_ = 0;
};

class Blocker {
public:
    Blocker() : release_gate_(release_.get_future().share()) {}

    ~Blocker() { release(); }

    Blocker(const Blocker&) = delete;
    Blocker& operator=(const Blocker&) = delete;

    void block() {
        started_.set_value();
        release_gate_.wait();
    }

    void wait_until_started(const char* message) {
        check(started_future_.wait_for(kWaitTimeout) == std::future_status::ready, message);
    }

    void release() {
        if (!released_.exchange(true)) release_.set_value();
    }

private:
    std::promise<void> started_;
    std::future<void> started_future_ = started_.get_future();
    std::promise<void> release_;
    std::shared_future<void> release_gate_;
    std::atomic<bool> released_ = false;
};

void expect_disabled_refresh(KnowledgeScheduler& scheduler) {
    try {
        (void)scheduler.refresh();
    } catch (const ServiceError& error) {
        check(error.status == 409 && error.code == "knowledge-disabled", "disabled refresh returned the wrong error");
        return;
    }
    throw std::runtime_error("disabled scheduler accepted refresh");
}

void wait_until_expired(KnowledgeScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (scheduler.snapshot(std::string(kRoute)).at("state") == "expired") return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("published healthy assessment did not expire");
}

void wait_until_idle(KnowledgeScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const Json status = scheduler.status();
        if (status.at("state") == "idle" && status.at("checks_completed") == 1) return;
        std::this_thread::yield();
    }
    throw std::runtime_error("scheduler did not reach the idle wait state");
}

void startup_and_read_only_snapshot_status() {
    CallCounter calls;
    Blocker second;
    KnowledgeScheduler scheduler(policy(true), targets(), [&](const KnowledgeTarget& target) {
        check(target.project_id == kProject && target.route_id == kRoute, "scheduler changed the inspection target");
        const int call = calls.enter();
        if (call == 1) return healthy_report("generation-one");
        if (call == 2) {
            second.block();
            return healthy_report("generation-two");
        }
        throw std::runtime_error("unexpected startup callback");
    });

    scheduler.start();
    check(calls.wait_for_at_least(1), "startup did not inspect before a request");
    const Json requested = scheduler.refresh();
    check(requested == Json({{"requested", true}, {"targets", 1}, {"completed_synchronously", false}}),
          "refresh acknowledgement changed");
    second.wait_until_started("startup refresh did not schedule a second inspection");

    const Json first_status = scheduler.status();
    const Json first_snapshot = scheduler.snapshot(std::string(kRoute));
    const Json second_status = scheduler.status();
    const Json second_snapshot = scheduler.snapshot(std::string(kRoute));
    check(first_status.at("enabled") == true && first_status.at("state") == "checking" &&
              first_status.at("active_route").get<std::string>() == kRoute && first_status.at("read_only") == true &&
              first_status.at("scope") == "process-local",
          "status did not expose a read-only active scheduler");
    check(first_snapshot.at("state") == "healthy" && first_snapshot.at("checking") == true &&
              first_snapshot.at("assessment").at("generation") == "generation-one" &&
              first_snapshot.at("assessment").at("mutations_performed") == false &&
              first_snapshot.at("read_only") == true && first_snapshot.at("exact_current_at_read") == false,
          "snapshot did not expose the prior read-only assessment");
    check(first_status.at("checks_completed") == second_status.at("checks_completed") &&
              first_status.at("refresh_requests") == second_status.at("refresh_requests") &&
              first_snapshot.at("assessment") == second_snapshot.at("assessment") && calls.value() == 2,
          "status or snapshot triggered a scheduler mutation");
    second.release();
}

void periodic_recheck() {
    CallCounter calls;
    KnowledgeScheduler scheduler(policy(true, 1, 2), targets(), [&](const KnowledgeTarget&) {
        const int call = calls.enter();
        return healthy_report("periodic-" + std::to_string(call));
    });
    scheduler.start();
    check(calls.wait_for_at_least(2), "scheduler did not perform a periodic recheck");
}

void configured_disabled_mode() {
    CallCounter calls;
    KnowledgeScheduler scheduler(policy(false), targets(), [&](const KnowledgeTarget&) {
        calls.enter();
        return healthy_report("unexpected");
    });
    scheduler.start();
    const Json status = scheduler.status();
    const Json snapshot = scheduler.snapshot(std::string(kRoute));
    check(calls.value() == 0 && status.at("state") == "disabled" && status.at("enabled") == false &&
              status.at("read_only") == true && snapshot.at("state") == "disabled" &&
              snapshot.at("assessment").is_null(),
          "disabled mode started maintenance or exposed a health report");
    expect_disabled_refresh(scheduler);
}

void refresh_coalesces_while_callback_blocks() {
    CallCounter calls;
    Blocker first;
    Blocker second;
    KnowledgeScheduler scheduler(policy(true), targets(), [&](const KnowledgeTarget&) {
        const int call = calls.enter();
        if (call == 1) {
            first.block();
            return healthy_report("generation-one");
        }
        if (call == 2) {
            second.block();
            return healthy_report("generation-two");
        }
        throw std::runtime_error("refresh coalescing scheduled duplicate callbacks");
    });
    scheduler.start();
    first.wait_until_started("first callback did not start");
    (void)scheduler.refresh();
    (void)scheduler.refresh();
    (void)scheduler.refresh();
    first.release();
    second.wait_until_started("coalesced refresh did not run one recheck");
    const Json status = scheduler.status();
    check(calls.value() == 2 && status.at("refresh_requests") == 3 && status.at("coalesced_requests") == 2 &&
              status.at("checks_completed") == 1,
          "blocked refreshes were not coalesced into one recheck");
    second.release();
}

void expiry_while_later_callback_blocks() {
    CallCounter calls;
    Blocker second;
    KnowledgeScheduler scheduler(policy(true, 1, 1), targets(), [&](const KnowledgeTarget&) {
        const int call = calls.enter();
        if (call == 1) return healthy_report("generation-one");
        if (call == 2) {
            second.block();
            return healthy_report("generation-two");
        }
        throw std::runtime_error("expiry test scheduled an unexpected callback");
    });
    scheduler.start();
    second.wait_until_started("periodic callback did not start for expiry test");
    wait_until_expired(scheduler);
    const Json snapshot = scheduler.snapshot(std::string(kRoute));
    check(snapshot.at("checking") == true && snapshot.at("state") == "expired" &&
              snapshot.at("assessment").at("generation") == "generation-one" &&
              snapshot.at("max_age_ms") == 1000,
          "blocked later callback refreshed or hid the expired assessment");
    second.release();
}

void failure_invalidates_old_healthy_state() {
    CallCounter calls;
    Blocker second;
    Blocker third;
    KnowledgeScheduler scheduler(policy(true), targets(), [&](const KnowledgeTarget&) {
        const int call = calls.enter();
        if (call == 1) return healthy_report("generation-one");
        if (call == 2) {
            second.block();
            throw ServiceError(503, "knowledge-inspection-failed", "fixture failure");
        }
        if (call == 3) {
            third.block();
            return healthy_report("generation-three");
        }
        throw std::runtime_error("failure test scheduled an unexpected callback");
    });
    scheduler.start();
    check(calls.wait_for_at_least(1), "initial healthy callback did not start");
    (void)scheduler.refresh();
    second.wait_until_started("failing callback did not start");
    (void)scheduler.refresh();
    second.release();
    third.wait_until_started("failure did not complete before the requested recheck");
    const Json snapshot = scheduler.snapshot(std::string(kRoute));
    const Json status = scheduler.status();
    check(snapshot.at("state") == "unavailable" && snapshot.at("assessment").at("healthy") == false &&
              snapshot.at("assessment").at("reason_codes") == Json::array({"knowledge-inspection-failed"}) &&
              status.at("checks_completed") == 2 && status.at("checks_failed") == 1,
          "failure retained the old healthy assessment");
    third.release();
}

void foreground_busy_defers_without_refreshing_age() {
    CallCounter calls;
    Blocker second;
    Blocker third;
    KnowledgeScheduler scheduler(policy(true), targets(), [&](const KnowledgeTarget&) -> Json {
        const int call = calls.enter();
        if (call == 1) return healthy_report("generation-one");
        if (call == 2) {
            second.block();
            throw ServiceError(503, "knowledge-foreground-busy", "fixture foreground");
        }
        if (call == 3) {
            third.block();
            return healthy_report("generation-three");
        }
        throw std::runtime_error("foreground busy test scheduled an unexpected callback");
    });
    scheduler.start();
    check(calls.wait_for_at_least(1), "initial foreground-busy fixture callback did not start");
    (void)scheduler.refresh();
    second.wait_until_started("foreground-busy callback did not start");
    const Json before = scheduler.snapshot(std::string(kRoute));
    (void)scheduler.refresh();
    second.release();
    third.wait_until_started("foreground-busy deferred recheck did not start");
    const Json after = scheduler.snapshot(std::string(kRoute));
    const Json status = scheduler.status();
    check(before.at("assessment").at("generation") == "generation-one" &&
              after.at("assessment") == before.at("assessment") &&
              after.at("observed_at_unix_ms") == before.at("observed_at_unix_ms") &&
              after.at("age_ms").get<long long>() >= before.at("age_ms").get<long long>() &&
              after.at("checking") == true && status.at("checks_completed") == 1 &&
              status.at("checks_deferred") == 1 && status.at("checks_failed") == 0,
          "foreground busy refreshed age or invalidated the prior healthy assessment");
    third.release();
}

void stop_joins_in_flight_callback() {
    Blocker callback;
    auto scheduler = std::make_unique<KnowledgeScheduler>(policy(true), targets(), [&](const KnowledgeTarget&) {
        callback.block();
        return healthy_report("generation-one");
    });
    scheduler->start();
    callback.wait_until_started("stop fixture callback did not start");
    auto stopped = std::async(std::launch::async, [&] {
        scheduler.reset();
        return true;
    });
    check(stopped.wait_for(200ms) == std::future_status::timeout, "scheduler stop did not join the in-flight callback");
    callback.release();
    check(stopped.wait_for(kWaitTimeout) == std::future_status::ready && stopped.get(),
          "scheduler stop did not complete after callback release");
}

void stop_wakes_idle_worker_with_long_interval() {
    CallCounter calls;
    auto scheduler = std::make_unique<KnowledgeScheduler>(policy(true, 600, 1200), targets(),
        [&](const KnowledgeTarget&) {
            calls.enter();
            return healthy_report("generation-one");
        });
    scheduler->start();
    check(calls.wait_for_at_least(1), "idle stop fixture callback did not start");
    wait_until_idle(*scheduler);
    auto stopped = std::async(std::launch::async, [&] {
        scheduler.reset();
        return true;
    });
    check(stopped.wait_for(kWaitTimeout) == std::future_status::ready && stopped.get() && calls.value() == 1,
          "idle worker did not stop promptly before its long interval deadline");
}

void early_stop_cancels_recheck_without_detaching() {
    Blocker callback;
    CallCounter calls;
    KnowledgeScheduler scheduler(policy(true), targets(), [&](const KnowledgeTarget&) {
        if (calls.enter() == 1) callback.block();
        return healthy_report("generation-one");
    });
    scheduler.start();
    callback.wait_until_started("early stop callback did not start");
    (void)scheduler.refresh();
    scheduler.request_stop();
    callback.release();
    wait_until_idle(scheduler);
    check(calls.value() == 1 && scheduler.status().at("checks_completed") == 1,
          "early stop started queued background work or discarded the in-flight result");
}

} // namespace

int main() {
    try {
        startup_and_read_only_snapshot_status();
        periodic_recheck();
        configured_disabled_mode();
        refresh_coalesces_while_callback_blocks();
        expiry_while_later_callback_blocks();
        failure_invalidates_old_healthy_state();
        foreground_busy_defers_without_refreshing_age();
        stop_joins_in_flight_callback();
        stop_wakes_idle_worker_with_long_interval();
        early_stop_cancels_recheck_without_detaching();
        std::cout << "knowledge scheduler startup, scheduling, expiry, coalescing and stop tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
