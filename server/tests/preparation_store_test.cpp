#include "shin/server/config.h"
#include "shin/server/store.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace shin::server;
namespace fs = std::filesystem;

constexpr std::uint64_t kReservationLimit = 1024ULL * 1024ULL * 1024ULL;
const std::string kRouteFingerprint(64, 'a');
const std::string kSnapshotId = "fnv1a64:0123456789abcdef";
const std::string kArtifactNamespace(64, 'a');
std::string_view active_check = "arguments";

class CheckFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void check(bool condition, const char* message) {
    if (!condition) {
        throw CheckFailure(message);
    }
}

std::uint64_t job_id(const Json& result) {
    const auto& job = result.at("job");
    check(job.is_object() && job.contains("id") && job.at("id").is_number_integer(), "preparation result is malformed");
    return job.at("id").get<std::uint64_t>();
}

void expects_error(const char* code, const std::function<void()>& action) {
    try {
        action();
    } catch (const ServiceError& error) {
        check(error.code == code, "preparation error code changed");
        return;
    }
    throw CheckFailure("preparation operation unexpectedly succeeded");
}

Principal configured_admin(const Config& config) {
    for (const auto& credential : config.credentials) {
        if (credential.principal.admin) {
            return credential.principal;
        }
    }
    throw std::runtime_error("configured administrator is unavailable");
}

std::string deterministic_artifact_key(const Principal& principal, std::string_view idempotency_key) {
    return digest_hex(sha256(principal.id + ":" + std::string(idempotency_key)));
}

Json enqueue(Store& store,
             const Principal& admin,
             std::string_view key,
             std::string_view request_hash,
             std::size_t retained_limit,
             std::uint64_t reservation_bytes = 1) {
    return store.enqueue_preparation(admin, "visible", "visible-retrieval", kRouteFingerprint, kSnapshotId, key,
                                     request_hash, retained_limit, reservation_bytes, kArtifactNamespace,
                                     [&] { return deterministic_artifact_key(admin, key); });
}

std::uint64_t enqueue_id(Store& store,
                         const Principal& admin,
                         std::string_view key,
                         std::string_view request_hash,
                         std::size_t retained_limit,
                         std::uint64_t reservation_bytes = 1) {
    const auto result = enqueue(store, admin, key, request_hash, retained_limit, reservation_bytes);
    check(result.at("replayed").is_boolean() && !result.at("replayed").get<bool>(), "new preparation replayed");
    return job_id(result);
}

Json claim_one(Store& store, const std::string& worker) {
    const auto claimed = store.claim_preparation(worker, 3);
    check(!claimed.is_null(), "queued preparation was not claimed");
    return claimed;
}

void finish_failed(Store& store, std::uint64_t id, const std::string& worker) {
    check(store.finish_preparation(id, worker, "failed", "test-failed", "", 0),
          "owner could not finish failed preparation");
}

std::size_t retained_count(Store& store, const Principal& admin) {
    const auto page = store.list_preparations(admin, 0, 100);
    check(page.at("items").is_array(), "preparation page is malformed");
    return page.at("items").size();
}

void check_idempotency_and_claim(Store& store, const Principal& admin, std::uint64_t& reserved_bytes) {
    const auto first = enqueue(store, admin, "ledger-idempotency", std::string(64, 'b'), 64);
    const auto id = job_id(first);
    ++reserved_bytes;
    const auto replay = enqueue(store, admin, "ledger-idempotency", std::string(64, 'b'), 64);
    check(replay.at("replayed").get<bool>() && job_id(replay) == id, "idempotency replay changed preparation identity");
    expects_error("idempotency-conflict", [&] {
        (void)enqueue(store, admin, "ledger-idempotency", std::string(64, 'c'), 64);
    });

    const std::string worker = "ledger-worker-idempotency";
    const auto claimed = claim_one(store, worker);
    check(claimed.at("id").get<std::uint64_t>() == id, "idempotency preparation claim changed identity");
    finish_failed(store, id, worker);
}

void check_atomic_admission_and_claim(const Config& config, const Principal& admin, std::uint64_t& reserved_bytes) {
    Store first{config.database};
    Store second{config.database};
    const auto count_before = retained_count(first, admin);
    check(count_before < 64, "preparation test cannot reserve another retained row");

    std::atomic<int> admitted{0};
    std::atomic<int> capacity_rejected{0};
    std::uint64_t admitted_id = 0;
    std::mutex admitted_mutex;
    std::atomic<bool> start{false};
    const auto submit = [&](Store& store, std::string_view key, char hash_character) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        try {
            const auto result = enqueue(store, admin, key, std::string(64, hash_character), count_before + 1);
            {
                std::lock_guard lock{admitted_mutex};
                admitted_id = job_id(result);
            }
            admitted.fetch_add(1, std::memory_order_relaxed);
        } catch (const ServiceError& error) {
            if (error.code == "preparation-capacity") {
                capacity_rejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            throw;
        }
    };

    std::exception_ptr first_error;
    std::exception_ptr second_error;
    std::thread first_thread([&] {
        try {
            submit(first, "ledger-concurrent-one", 'd');
        } catch (...) {
            first_error = std::current_exception();
        }
    });
    std::thread second_thread([&] {
        try {
            submit(second, "ledger-concurrent-two", 'e');
        } catch (...) {
            second_error = std::current_exception();
        }
    });
    start.store(true, std::memory_order_release);
    first_thread.join();
    second_thread.join();
    if (first_error) {
        std::rethrow_exception(first_error);
    }
    if (second_error) {
        std::rethrow_exception(second_error);
    }
    check(admitted.load() == 1 && capacity_rejected.load() == 1 && admitted_id != 0,
          "concurrent retained admission was not atomic");
    ++reserved_bytes;

    Store claim_first{config.database};
    Store claim_second{config.database};
    Json first_claim = nullptr;
    Json second_claim = nullptr;
    std::thread first_claim_thread([&] { first_claim = claim_first.claim_preparation("ledger-worker-claim-one", 3); });
    std::thread second_claim_thread([&] { second_claim = claim_second.claim_preparation("ledger-worker-claim-two", 3); });
    first_claim_thread.join();
    second_claim_thread.join();
    const auto claimed_count = static_cast<int>(!first_claim.is_null()) + static_cast<int>(!second_claim.is_null());
    check(claimed_count == 1, "preparation claim was not exclusive");
    const auto& claimed = first_claim.is_null() ? second_claim : first_claim;
    check(claimed.at("id").get<std::uint64_t>() == admitted_id, "exclusive claim selected the wrong preparation");
    finish_failed(claim_first, admitted_id,
                  first_claim.is_null() ? "ledger-worker-claim-two" : "ledger-worker-claim-one");
}

void check_owner_and_cancellation(Store& store, const Principal& admin, std::uint64_t& reserved_bytes) {
    const std::string owner = "ledger-worker-owner";
    const std::string wrong_owner = "ledger-worker-wrong-owner";
    const auto owner_id = enqueue_id(store, admin, "ledger-owner", std::string(64, 'f'), 64);
    ++reserved_bytes;
    check(claim_one(store, owner).at("id").get<std::uint64_t>() == owner_id, "owner test claimed wrong preparation");
    check(!store.renew_preparation(owner_id, wrong_owner, 3), "wrong owner renewed preparation");
    check(!store.finish_preparation(owner_id, wrong_owner, "failed", "test-failed", "", 0),
          "wrong owner finished preparation");
    finish_failed(store, owner_id, owner);

    const std::string cancel_worker = "ledger-worker-cancel";
    const auto running_id = enqueue_id(store, admin, "ledger-running-cancel", std::string(64, '1'), 64);
    ++reserved_bytes;
    check(claim_one(store, cancel_worker).at("id").get<std::uint64_t>() == running_id,
          "running cancellation claimed wrong preparation");
    const auto cancelled_running = store.cancel_preparation(admin, running_id);
    check(cancelled_running.at("state") == "running" && cancelled_running.at("cancel_requested") == true,
          "running cancellation was not recorded");
    check(store.finish_preparation(running_id, cancel_worker, "prepared", "", kSnapshotId, 1),
          "cancelled worker could not acknowledge completion");
    const auto final_running = store.get_preparation(admin, running_id);
    check(final_running.at("state") == "cancelled" && final_running.at("artifact_snapshot_id").is_null(),
          "running cancellation allowed a prepared artifact");

    const auto queued_id = enqueue_id(store, admin, "ledger-queued-cancel", std::string(64, '2'), 64);
    ++reserved_bytes;
    const auto queued_cancel = store.cancel_preparation(admin, queued_id);
    check(queued_cancel.at("state") == "cancelled" && queued_cancel.at("reason_code") == "cancelled",
          "queued cancellation did not become terminal");
    check(store.claim_preparation("ledger-worker-queued-check", 3).is_null(),
          "queued cancellation remained claimable");
}

void check_expiry(Store& store, const Principal& admin, std::uint64_t& reserved_bytes) {
    const std::string owner = "ledger-worker-expiry";
    const auto id = enqueue_id(store, admin, "ledger-expiry", std::string(64, '3'), 64);
    ++reserved_bytes;
    check(claim_one(store, owner).at("id").get<std::uint64_t>() == id, "expiry test claimed wrong preparation");
    store.expire_preparations();
    check(store.get_preparation(admin, id).at("state") == "running", "unexpired preparation was expired");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    Json expired;
    // The lease uses the database wall clock, which can step independently of this process's sleep clock.
    do {
        store.expire_preparations();
        expired = store.get_preparation(admin, id);
        if (expired.at("state") != "running" || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (true);
    check(expired.at("state") == "failed" && expired.at("reason_code") == "lease-expired",
          "expired preparation did not become lease-expired failure");
    check(!store.finish_preparation(id, owner, "prepared", "", kSnapshotId, 1),
          "stale owner finished expired preparation");
}

void check_retention(Store& store, const Principal& admin, std::uint64_t& reserved_bytes) {
    const auto public_history_before = retained_count(store, admin);
    const std::string request_hash(64, '8');
    const std::uint64_t reservation_bytes = 7;
    const auto id = enqueue_id(store, admin, "ledger-retention", request_hash, public_history_before + 1,
                               reservation_bytes);
    reserved_bytes += reservation_bytes;
    const std::string worker = "ledger-worker-retention";
    check(claim_one(store, worker).at("id").get<std::uint64_t>() == id, "retention test claimed the wrong preparation");
    finish_failed(store, id, worker);

    const auto expected_input = store.preparation_inspection_input(admin, id);
    check(expected_input.at("artifact_namespace") == kArtifactNamespace &&
              expected_input.at("artifact_key") == deterministic_artifact_key(admin, "ledger-retention") &&
              expected_input.at("worker_id") == worker && expected_input.at("retention").is_null(),
          "retention input did not retain the internal preparation binding");
    const auto public_before = store.get_preparation(admin, id);
    const std::string plan_id(64, '9');
    const Json inventory{{"root_present", false}, {"entries", Json::array()}, {"payload_bytes", 0}};
    auto changed_input = expected_input;
    changed_input["worker_id"] = nullptr;
    expects_error("retention-job-changed", [&] {
        (void)store.begin_retention(admin, id, changed_input, plan_id, inventory);
    });
    const auto intent = store.begin_retention(admin, id, expected_input, plan_id, inventory);
    check(intent.at("preparation_id").get<std::uint64_t>() == id && intent.at("state") == "intent" &&
              intent.at("plan_id") == plan_id && intent.at("inventory") == inventory &&
              intent.at("requested_by") == admin.id && intent.at("completed_by").is_null() &&
              intent.at("released_at").is_null(),
          "retention intent was not stored as an internal record");
    check(store.get_preparation(admin, id) == public_before, "retention intent changed the public preparation ledger");

    expects_error("preparation-capacity", [&] {
        (void)enqueue(store, admin, "ledger-retention-capacity", std::string(64, 'a'), public_history_before + 1);
    });
    expects_error("retention-plan-conflict", [&] {
        (void)store.begin_retention(admin, id, expected_input, std::string(64, 'b'), inventory);
    });

    Principal reader = admin;
    reader.admin = false;
    reader.project_ids = {"visible"};
    expects_error("forbidden", [&] { (void)store.begin_retention(reader, id, expected_input, plan_id, inventory); });
    expects_error("forbidden", [&] {
        (void)store.finish_retention(reader, id, plan_id, kArtifactNamespace,
                                    deterministic_artifact_key(admin, "ledger-retention"));
    });
    expects_error("retention-plan-conflict", [&] {
        (void)store.finish_retention(admin, id, std::string(64, 'b'), kArtifactNamespace,
                                    deterministic_artifact_key(admin, "ledger-retention"));
    });
    expects_error("retention-job-changed", [&] {
        (void)store.finish_retention(admin, id, plan_id, kArtifactNamespace, std::string(64, 'e'));
    });

    const auto released = store.finish_retention(admin, id, plan_id, kArtifactNamespace,
                                                 deterministic_artifact_key(admin, "ledger-retention"));
    check(released.at("state") == "released" && released.at("completed_by") == admin.id &&
              released.at("released_at").is_string(),
          "retention release did not record its completing administrator");
    check(store.finish_retention(admin, id, plan_id, kArtifactNamespace,
                                 deterministic_artifact_key(admin, "ledger-retention")) == released,
          "retention release replay changed its internal record");
    check(store.get_preparation(admin, id) == public_before, "retention release changed the public preparation ledger");

    const auto replay = store.enqueue_preparation(
        admin, "visible", "visible-retrieval", kRouteFingerprint, kSnapshotId, "ledger-retention", request_hash,
        public_history_before + 1, reservation_bytes, kArtifactNamespace, []() -> std::string {
            throw std::runtime_error("idempotent replay called the failing entropy source");
        });
    check(replay.at("replayed") == true && job_id(replay) == id,
          "released preparation idempotency did not replay with a new artifact key");
    reserved_bytes -= reservation_bytes;

    (void)enqueue_id(store, admin, "ledger-retention-reclaimed", std::string(64, 'd'), public_history_before + 1);
    ++reserved_bytes;
}

void check_retained_and_reservation_bounds(Store& store, const Principal& admin, std::uint64_t reserved_bytes) {
    const auto before_count = retained_count(store, admin);
    check(before_count > 0 && before_count < 64, "preparation retained test exceeded the configured maximum");
    // check_retention leaves one released historical row outside the active quota.
    (void)enqueue_id(store, admin, "ledger-retained-bound", std::string(64, '4'), before_count);
    ++reserved_bytes;
    expects_error("preparation-capacity", [&] {
        (void)enqueue(store, admin, "ledger-retained-reject", std::string(64, '5'), before_count);
    });

    check(reserved_bytes < kReservationLimit, "reservation accounting exceeded the global limit before fill");
    (void)enqueue_id(store, admin, "ledger-reservation-fill", std::string(64, '6'), 64,
                     kReservationLimit - reserved_bytes);
    expects_error("preparation-capacity", [&] {
        (void)enqueue(store, admin, "ledger-reservation-reject", std::string(64, '7'), 64);
    });
    check(retained_count(store, admin) <= 64, "retained preparations exceeded the configured maximum");
}

void run(const fs::path& config_path) {
    active_check = "configuration";
    check(config_path.is_absolute(), "configuration path must be absolute");
    const Config config = load_config(config_path);
    const Principal admin = configured_admin(config);
    Store store{config.database};
    active_check = "empty-ledger";
    check(retained_count(store, admin) == 0, "preparation table must be empty before the native probe");

    std::uint64_t reserved_bytes = 0;
    active_check = "idempotency";
    check_idempotency_and_claim(store, admin, reserved_bytes);
    active_check = "atomic-admission-and-claim";
    check_atomic_admission_and_claim(config, admin, reserved_bytes);
    active_check = "ownership-and-cancellation";
    check_owner_and_cancellation(store, admin, reserved_bytes);
    active_check = "lease-expiry";
    check_expiry(store, admin, reserved_bytes);
    active_check = "retention";
    check_retention(store, admin, reserved_bytes);
    active_check = "retained-and-reservation-bounds";
    check_retained_and_reservation_bounds(store, admin, reserved_bytes);
}

void report(bool ok) {
    if (ok) {
        std::cout << "{\"ok\":true,\"checks\":[\"idempotency\",\"atomic-admission\",\"claim-exclusivity\","
                     "\"ownership\",\"cancellation\",\"lease-expiry\",\"retention\","
                     "\"retained-and-reservation-bounds\"]}\n";
    } else {
        std::cout << "{\"ok\":false,\"checks\":[]}\n";
    }
}

void report_failure(std::string_view kind, std::string_view detail = {}) {
    // Only public error codes or this probe's own literal assertions, never SQL/connection exception text.
    try {
        std::cout << Json{{"ok", false}, {"checks", Json::array()},
            {"failure", {{"stage", active_check}, {"kind", kind}, {"detail", detail}}}}.dump() << '\n';
    } catch (...) {
        report(false);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        report(false);
        return 2;
    }
    try {
        run(fs::path{argv[1]});
        report(true);
        return 0;
    } catch (const CheckFailure& error) {
        report_failure("assertion", error.what());
        return 1;
    } catch (const ServiceError& error) {
        report_failure("service-error", error.code);
        return 1;
    } catch (...) {
        report_failure("unexpected-exception");
        return 1;
    }
}
