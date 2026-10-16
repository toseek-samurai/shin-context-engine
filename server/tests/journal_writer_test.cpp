#include "shin/server/journal_writer.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
constexpr std::string_view kInstance = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

JournalWriterConfig policy(bool enabled = true, int interval_ms = 100, int max_retry_ms = 100,
                           bool flush_on_shutdown = true) {
    JournalWriterConfig result;
    result.enabled = enabled;
    result.principal_id = enabled ? "administrator" : "";
    result.interval_ms = interval_ms;
    result.max_retry_ms = max_retry_ms;
    result.flush_on_shutdown = flush_on_shutdown;
    return result;
}

Json batch(std::uint64_t latest, Json records = Json::array()) {
    return {{"instance_id", std::string(kInstance)}, {"latest_sequence", latest}, {"records", std::move(records)}};
}

Json receipt(std::uint64_t through, std::uint64_t epoch = 7, std::uint64_t inserted = 0,
             std::uint64_t missing = 0, std::uint64_t recovered = 0) {
    return {{"schema_version", "shin.retrieval-journal-capture.v1"}, {"instance_id", std::string(kInstance)},
            {"observed_through", through}, {"producer_epoch", epoch}, {"inserted_count", inserted},
            {"new_missing_sequences", missing}, {"recovered_missing_sequences", recovered}};
}

class CallCounter {
public:
    int enter() {
        std::lock_guard lock(mutex_);
        const int result = value_.fetch_add(1) + 1;
        wake_.notify_all();
        return result;
    }

    bool wait_for_at_least(int expected) {
        std::unique_lock lock(mutex_);
        return wake_.wait_for(lock, kWaitTimeout, [&] { return value_.load() >= expected; });
    }

    int value() const { return value_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<int> value_ = 0;
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

class ReleaseGuard {
public:
    explicit ReleaseGuard(Blocker& blocker) : blocker_(blocker) {}
    ~ReleaseGuard() { blocker_.release(); }

    ReleaseGuard(const ReleaseGuard&) = delete;
    ReleaseGuard& operator=(const ReleaseGuard&) = delete;

private:
    Blocker& blocker_;
};

template <typename Predicate>
Json wait_for_status(JournalWriter& writer, Predicate&& predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        Json value = writer.status();
        if (predicate(value)) return value;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error(message);
}

void disabled_and_empty_batches_do_not_export() {
    CallCounter snapshots;
    CallCounter exports;
    JournalWriter disabled(policy(false), [&](std::uint64_t) {
        snapshots.enter();
        return batch(1);
    }, [&](const Json&) {
        exports.enter();
        return receipt(1);
    });
    disabled.start();
    const Json before_shutdown = disabled.status();
    disabled.shutdown();
    check(snapshots.value() == 0 && exports.value() == 0 && before_shutdown.at("state") == "disabled" &&
              before_shutdown.at("pending_batches") == 0 && before_shutdown.at("read_only") == true,
          "disabled writer polled or exported data");

    CallCounter empty_snapshots;
    CallCounter empty_exports;
    JournalWriter empty(policy(), [&](std::uint64_t cursor) {
        check(cursor == 0, "empty writer changed the acknowledgement cursor");
        empty_snapshots.enter();
        return batch(0);
    }, [&](const Json&) {
        empty_exports.enter();
        return receipt(0);
    });
    empty.start();
    check(empty_snapshots.wait_for_at_least(1), "empty writer did not poll its source");
    empty.shutdown();
    check(empty_exports.value() == 0 && empty.status().at("acknowledged_through") == 0,
          "empty source progress enrolled or exported a producer");
}

void retry_retains_the_original_payload() {
    CallCounter exports;
    CallCounter snapshots;
    std::mutex payload_mutex;
    std::vector<Json> payloads;
    JournalWriter writer(policy(), [&](std::uint64_t cursor) {
        const int call = snapshots.enter();
        if (call > 1) {
            check(cursor == 9, "post-retry snapshot did not use the acknowledged prefix");
            return batch(9);
        }
        check(cursor == 0, "retry snapshot used an acknowledgement before a successful receipt");
        return batch(9, Json::array({{{"source_version", call}}}));
    }, [&](const Json& value) {
        const int call = exports.enter();
        {
            std::lock_guard lock(payload_mutex);
            payloads.push_back(value);
        }
        if (call == 1) throw ServiceError(503, "journal-store-unavailable", "transient fixture failure");
        return receipt(9, 11, 1);
    });

    writer.start();
    check(exports.wait_for_at_least(2), "writer did not retry the pending batch");
    const Json after_success = wait_for_status(writer, [](const Json& value) {
        return value.at("export_successes") == 1;
    }, "retry did not publish success");
    {
        std::lock_guard lock(payload_mutex);
        check(snapshots.value() == 1 && payloads.size() == 2 && payloads[0] == payloads[1] &&
                  after_success.at("pending_batches") == 0 && after_success.at("acknowledged_through") == 9 &&
                  after_success.at("acknowledged_producer_epoch") == 11,
              "retry did not preserve the original immutable batch");
    }
    writer.shutdown();
}

void callbacks_are_serial_and_retry_delay_is_capped() {
    CallCounter exports;
    std::atomic<int> active = 0;
    std::atomic<int> maximum_active = 0;
    const auto enter_callback = [&] {
        const int current = active.fetch_add(1) + 1;
        int observed = maximum_active.load();
        while (observed < current && !maximum_active.compare_exchange_weak(observed, current)) {}
    };
    const auto leave_callback = [&] { active.fetch_sub(1); };
    JournalWriter writer(policy(true, 100, 100), [&](std::uint64_t) {
        enter_callback();
        leave_callback();
        return batch(1, Json::array({{{"sequence", 1}}}));
    }, [&](const Json&) {
        enter_callback();
        const int call = exports.enter();
        leave_callback();
        if (call < 3) throw ServiceError(503, "journal-store-unavailable", "transient fixture failure");
        return receipt(1, 3, 1);
    });

    writer.start();
    for (int failures = 1; failures <= 2; ++failures) {
        const Json waiting = wait_for_status(writer, [failures](const Json& value) {
            return value.at("state") == "retry-wait" && value.at("consecutive_failures") == failures;
        }, "writer did not enter retry wait");
        check(waiting.at("next_attempt_in_ms").get<std::int64_t>() <= 100,
              "writer retry delay exceeded its configured cap");
    }
    check(exports.wait_for_at_least(3), "writer did not perform serialized retries");
    (void)wait_for_status(writer, [](const Json& value) { return value.at("export_successes") == 1; },
                          "writer did not recover after capped retries");
    writer.shutdown();
    check(maximum_active.load() == 1 && active.load() == 0,
          "writer invoked snapshot or exporter callbacks concurrently");
}

void idle_snapshot_recovers_from_a_transient_failure() {
    CallCounter snapshots;
    CallCounter exports;
    JournalWriter writer(policy(), [&](std::uint64_t cursor) {
        check(cursor == 0, "idle recovery changed the acknowledgement cursor");
        if (snapshots.enter() == 1) throw std::runtime_error("transient snapshot fixture failure");
        return batch(0);
    }, [&](const Json&) {
        exports.enter();
        return receipt(0);
    });

    writer.start();
    check(snapshots.wait_for_at_least(2), "writer did not retry a transient snapshot failure");
    const Json status = wait_for_status(writer, [](const Json& value) {
        return value.at("state") == "waiting" && value.at("failures") == 1 &&
            value.at("consecutive_failures") == 0 && value.at("last_failure").is_null();
    }, "successful idle snapshot did not clear transient failure state");
    writer.shutdown();
    check(exports.value() == 0 && status.at("export_successes") == 0 &&
              status.at("acknowledged_producer_epoch").is_null() && status.at("next_attempt_in_ms").is_number_integer(),
          "idle recovery exported data or did not schedule the next poll");
}

void polls_with_the_acknowledged_cursor() {
    CallCounter snapshots;
    JournalWriter writer(policy(), [&](std::uint64_t cursor) {
        const int call = snapshots.enter();
        if (call == 1) {
            check(cursor == 0, "first poll did not start at cursor zero");
            return batch(3, Json::array({{{"sequence", 3}}}));
        }
        check(cursor == 8, "subsequent poll did not receive the acknowledged prefix");
        return batch(8);
    }, [&](const Json&) {
        return receipt(8, 2, 1, 5);
    });

    writer.start();
    check(snapshots.wait_for_at_least(2), "writer did not poll again after its export receipt");
    const Json status = wait_for_status(writer, [](const Json& value) { return value.at("sampled_through") == 8; },
                                        "second source poll did not publish its progress");
    writer.shutdown();
    check(status.at("acknowledged_through") == 8 && status.at("sampled_through") == 8 &&
              status.at("acknowledgement_includes_missing") == true && status.at("new_missing_sequences") == 5,
          "writer did not expose acknowledged missing-sequence progress");
}

void malformed_data_and_bounds_fail_closed() {
    const auto expect_blocked_snapshot = [](Json source, const char* message) {
        CallCounter exports;
        JournalWriter writer(policy(), [source = std::move(source)](std::uint64_t) { return source; }, [&](const Json&) {
            exports.enter();
            return receipt(1);
        });
        writer.start();
        const Json status = wait_for_status(writer, [](const Json& value) { return value.at("state") == "blocked"; }, message);
        writer.shutdown();
        check(exports.value() == 0 && status.at("last_failure").at("stage") == "snapshot" &&
                  status.at("pending_batches") == 0,
              "malformed snapshot did not fail closed before export");
    };

    expect_blocked_snapshot(Json::object(), "missing snapshot fields did not block the writer");
    expect_blocked_snapshot(batch(1, Json(std::vector<Json>(257, Json::object()))),
                            "record-count bound did not block the writer");
    Json oversized = batch(1);
    oversized["padding"] = std::string(JournalWriter::maximum_batch_bytes, 'x');
    expect_blocked_snapshot(std::move(oversized), "encoded batch bound did not block the writer");

    const auto expect_blocked_receipt = [](Json response, const char* message) {
        CallCounter exports;
        JournalWriter writer(policy(), [](std::uint64_t) { return batch(1, Json::array({{{"sequence", 1}}})); },
            [&](const Json&) {
                exports.enter();
                return response;
            });
        writer.start();
        const Json status = wait_for_status(writer, [](const Json& value) { return value.at("state") == "blocked"; }, message);
        writer.shutdown();
        check(exports.value() == 1 && status.at("last_failure").at("stage") == "receipt" &&
                  status.at("last_failure").at("code") == "journal-writer-invalid-receipt",
              "malformed receipt did not fail closed");
    };
    Json missing_schema = receipt(1, 7, 1);
    missing_schema.erase("schema_version");
    expect_blocked_receipt(std::move(missing_schema), "receipt without schema version did not block the writer");
    Json wrong_schema = receipt(1, 7, 1);
    wrong_schema["schema_version"] = "shin.retrieval-journal-capture.v0";
    expect_blocked_receipt(std::move(wrong_schema), "receipt with wrong schema version did not block the writer");
}

void reclaimed_producer_is_terminal_and_private() {
    CallCounter snapshots;
    CallCounter exports;
    JournalWriter writer(policy(), [&](std::uint64_t) {
        snapshots.enter();
        return batch(1, Json::array({{{"sequence", 1}}}));
    }, [&](const Json&) -> Json {
        exports.enter();
        throw ServiceError(409, "journal-producer-reclaimed", "private provider detail must not reach status");
    });
    writer.start();
    const Json status = wait_for_status(writer, [](const Json& value) { return value.at("state") == "blocked"; },
                                        "reclaimed producer did not block the writer");
    writer.shutdown();
    check(snapshots.value() == 1 && exports.value() == 1 && status.at("last_failure") ==
              Json({{"code", "journal-producer-reclaimed"}, {"stage", "export"}}) &&
              status.dump().find("private provider detail") == std::string::npos,
          "reclaimed producer was retried, reenrolled, or leaked private failure detail");
}

void normalizer_failure_is_terminal_without_shutdown_retry() {
    CallCounter snapshots;
    CallCounter exports;
    JournalWriter writer(policy(true, 60000, 60000), [&](std::uint64_t) {
        snapshots.enter();
        return batch(1, Json::array({{{"sequence", 1}}}));
    }, [&](const Json&) -> Json {
        exports.enter();
        throw ServiceError(500, "journal-invalid-batch", "normalizer fixture rejected the batch");
    });
    writer.start();
    const Json status = wait_for_status(writer, [](const Json& value) { return value.at("state") == "blocked"; },
                                        "normalizer failure did not block the writer");
    writer.shutdown();
    check(snapshots.value() == 1 && exports.value() == 1 && status.at("failures") == 1 &&
              status.at("last_failure") == Json({{"code", "journal-invalid-batch"}, {"stage", "export"}}) &&
              status.at("pending_batches") == 1 && status.at("shutdown_prefix_acknowledged") == false,
          "terminal normalizer failure retried or acknowledged pending data during shutdown");
}

void receipt_epoch_is_validated_after_the_first_capture() {
    CallCounter snapshots;
    CallCounter exports;
    JournalWriter writer(policy(), [&](std::uint64_t cursor) {
        const int call = snapshots.enter();
        if (call == 1) {
            check(cursor == 0, "first epoch fixture snapshot did not start at zero");
            return batch(1, Json::array({{{"sequence", 1}}}));
        }
        check(call == 2 && cursor == 1, "epoch fixture did not poll the acknowledged prefix");
        return batch(2, Json::array({{{"sequence", 2}}}));
    }, [&](const Json& value) {
        check(!value.contains("producer_epoch"), "snapshot batch improperly supplied a producer epoch");
        const int call = exports.enter();
        if (call == 1) return receipt(1, 17, 1);
        if (call == 2) return receipt(2, 18, 1);
        throw std::runtime_error("epoch fixture scheduled an unexpected export");
    });
    writer.start();
    check(exports.wait_for_at_least(2), "epoch fixture did not reach the changed receipt epoch");
    const Json status = wait_for_status(writer, [](const Json& value) { return value.at("state") == "blocked"; },
                                        "changed receipt epoch did not block the writer");
    writer.shutdown();
    check(snapshots.value() == 2 && exports.value() == 2 && status.at("export_successes") == 1 &&
              status.at("acknowledged_through") == 1 && status.at("acknowledged_producer_epoch") == 17 &&
              status.at("last_failure") == Json({{"code", "journal-writer-invalid-receipt"}, {"stage", "receipt"}}),
          "writer did not preserve the first validated receipt epoch or block its change");
}

void status_is_read_only_private_and_concurrent() {
    CallCounter snapshots;
    Blocker exporter;
    JournalWriter writer(policy(true, 60000, 60000), [&](std::uint64_t) {
        snapshots.enter();
        return batch(1, Json::array({{{"query", "private-query"}, {"authorization", "private-token"}}}));
    }, [&](const Json&) {
        exporter.block();
        return receipt(1, 6, 1);
    });
    ReleaseGuard release_on_exit(exporter);
    writer.start();
    exporter.wait_until_started("exporter did not enter the concurrent-status fixture");

    constexpr int readers = 6;
    std::barrier begin(readers + 1);
    std::vector<std::future<Json>> reads;
    reads.reserve(readers);
    for (int i = 0; i < readers; ++i) {
        reads.push_back(std::async(std::launch::async, [&] {
            begin.arrive_and_wait();
            Json result;
            for (int read = 0; read < 32; ++read) result = writer.status();
            return result;
        }));
    }
    begin.arrive_and_wait();
    for (auto& read : reads) {
        check(read.wait_for(kWaitTimeout) == std::future_status::ready, "concurrent status call did not complete");
        const Json status = read.get();
        check(status.at("read_only") == true && status.at("scope") == "process-lifetime" &&
                  status.at("state") == "exporting" && status.dump().find("private-query") == std::string::npos &&
                  status.dump().find("private-token") == std::string::npos && !status.contains("records") &&
                  !status.contains("instance_id"),
              "status was mutating, incomplete, or exposed payload data");
    }
    check(snapshots.value() == 1, "status triggered an additional source poll");
    exporter.release();
    writer.shutdown();
}

void stop_before_start_prevents_restart() {
    JournalWriter writer(policy(), [](std::uint64_t) { return batch(1); }, [](const Json&) { return receipt(1); });
    const Json before_start = writer.status();
    check(before_start.at("state") == "not-started" && before_start.at("next_attempt_in_ms").is_null(),
          "not-started writer advertised a retry deadline");
    writer.shutdown();
    check(writer.status().at("state") == "stopped", "stop-before-start did not publish stopped state");
    try {
        writer.start();
    } catch (const ServiceError& error) {
        check(error.status == 409 && error.code == "journal-writer-already-started",
              "stop-before-start returned the wrong restart error");
        return;
    }
    throw std::runtime_error("writer restarted after shutdown");
}

void shutdown_joins_in_flight_callback() {
    Blocker exporter;
    auto writer = std::make_unique<JournalWriter>(policy(true, 60000, 60000),
        [](std::uint64_t) { return batch(1, Json::array({{{"sequence", 1}}})); }, [&](const Json&) {
            exporter.block();
            return receipt(1, 9, 1);
        });
    std::promise<void> stopped_signal;
    auto stopped = stopped_signal.get_future();
    std::unique_ptr<std::jthread> stopper;
    ReleaseGuard release_on_exit(exporter);
    writer->start();
    exporter.wait_until_started("shutdown fixture exporter did not start");
    stopper = std::make_unique<std::jthread>([&] {
        writer.reset();
        stopped_signal.set_value();
    });
    check(stopped.wait_for(100ms) == std::future_status::timeout,
          "writer destruction detached the in-flight callback instead of joining it");
    exporter.release();
    check(stopped.wait_for(kWaitTimeout) == std::future_status::ready,
          "writer destruction did not finish after the callback returned");
    stopped.get();
}

void shutdown_flushes_pending_and_final_tail_with_two_exports() {
    CallCounter snapshots;
    std::mutex exports_mutex;
    std::vector<Json> exports;
    JournalWriter writer(policy(true, 60000, 60000), [&](std::uint64_t) {
        const int call = snapshots.enter();
        return batch(static_cast<std::uint64_t>(std::min(call, 3)),
                     Json::array({{{"sequence", std::min(call, 3)}}}));
    }, [&](const Json& value) {
        std::lock_guard lock(exports_mutex);
        exports.push_back(value);
        return receipt(value.at("latest_sequence").get<std::uint64_t>(), 4, 1);
    });
    writer.start();
    (void)wait_for_status(writer, [](const Json& value) { return value.at("export_successes") == 1; },
                          "initial export did not finish before shutdown");
    writer.shutdown();
    const Json status = writer.status();
    std::lock_guard lock(exports_mutex);
    check(exports.size() == 3 && exports[0].at("latest_sequence") == 1 && exports[1].at("latest_sequence") == 2 &&
              exports[2].at("latest_sequence") == 3 && snapshots.value() == 4 &&
              status.at("shutdown_prefix_acknowledged") == true,
          "shutdown did not capture pending data and one final tail within two flush exports");
}

void shutdown_does_not_retry_an_in_flight_failure() {
    Blocker exporter;
    CallCounter exports;
    JournalWriter writer(policy(true, 60000, 60000),
        [](std::uint64_t) { return batch(1, Json::array({{{"sequence", 1}}})); },
        [&](const Json&) -> Json {
            if (exports.enter() == 1) exporter.block();
            throw ServiceError(503, "database-unavailable", "controlled shutdown failure");
        });
    std::promise<void> stopped_signal;
    auto stopped = stopped_signal.get_future();
    std::unique_ptr<std::jthread> stopper;
    ReleaseGuard release_on_exit(exporter);
    writer.start();
    exporter.wait_until_started("failed shutdown exporter did not start");
    stopper = std::make_unique<std::jthread>([&] {
        writer.shutdown();
        stopped_signal.set_value();
    });
    check(stopped.wait_for(100ms) == std::future_status::timeout,
          "shutdown did not join the blocked failing exporter");
    exporter.release();
    check(stopped.wait_for(kWaitTimeout) == std::future_status::ready,
          "shutdown did not finish after the failing exporter returned");
    stopped.get();
    const auto status = writer.status();
    check(exports.value() == 1 && status.at("pending_batches") == 1 &&
              status.at("acknowledged_through") == 0 && status.at("shutdown_prefix_acknowledged") == false,
          "shutdown retried an in-flight failure or fabricated an acknowledgement");
}

void shutdown_failure_and_disabled_flush_are_explicit() {
    CallCounter failures;
    JournalWriter failed_writer(policy(true, 60000, 60000), [](std::uint64_t) {
        return batch(1, Json::array({{{"sequence", 1}}}));
    }, [&](const Json&) -> Json {
        failures.enter();
        throw ServiceError(503, "journal-store-unavailable", "transient fixture failure");
    });
    failed_writer.start();
    check(failures.wait_for_at_least(1), "failure fixture did not attempt its initial export");
    (void)wait_for_status(failed_writer, [](const Json& value) { return value.at("state") == "retry-wait"; },
                          "initial failure did not finish before shutdown");
    failed_writer.shutdown();
    const Json failed_status = failed_writer.status();
    check(failures.value() == 2 && failed_status.at("pending_batches") == 1 &&
              failed_status.at("shutdown_prefix_acknowledged") == false && failed_status.at("state") == "stopped",
          "failed shutdown did not retain pending work and an explicit incomplete prefix");

    CallCounter disabled_flush_exports;
    CallCounter disabled_flush_snapshots;
    JournalWriter no_flush(policy(true, 60000, 60000, false), [&](std::uint64_t) {
        const int call = disabled_flush_snapshots.enter();
        return batch(static_cast<std::uint64_t>(call), Json::array({{{"sequence", call}}}));
    }, [&](const Json& value) {
        disabled_flush_exports.enter();
        return receipt(value.at("latest_sequence").get<std::uint64_t>(), 5, 1);
    });
    no_flush.start();
    (void)wait_for_status(no_flush, [](const Json& value) { return value.at("export_successes") == 1; },
                          "flush-disabled fixture did not complete its normal export");
    no_flush.shutdown();
    const Json no_flush_status = no_flush.status();
    check(disabled_flush_exports.value() == 1 && no_flush_status.at("pending_batches") == 1 &&
              no_flush_status.at("shutdown_prefix_acknowledged") == false,
          "flush-disabled shutdown exported additional pending data");
}

} // namespace

int main() {
    try {
        disabled_and_empty_batches_do_not_export();
        retry_retains_the_original_payload();
        callbacks_are_serial_and_retry_delay_is_capped();
        idle_snapshot_recovers_from_a_transient_failure();
        polls_with_the_acknowledged_cursor();
        malformed_data_and_bounds_fail_closed();
        reclaimed_producer_is_terminal_and_private();
        normalizer_failure_is_terminal_without_shutdown_retry();
        receipt_epoch_is_validated_after_the_first_capture();
        status_is_read_only_private_and_concurrent();
        stop_before_start_prevents_restart();
        shutdown_joins_in_flight_callback();
        shutdown_flushes_pending_and_final_tail_with_two_exports();
        shutdown_does_not_retry_an_in_flight_failure();
        shutdown_failure_and_disabled_flush_are_explicit();
        std::cout << "journal writer lifecycle, bounds, retry, privacy and shutdown tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
