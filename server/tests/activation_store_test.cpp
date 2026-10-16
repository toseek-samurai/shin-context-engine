#include "shin/server/config.h"
#include "shin/server/store.h"

#include <pqxx/pqxx>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace shin::server;
namespace fs = std::filesystem;

constexpr std::string_view kProjectId = "activation-native";
constexpr std::string_view kRouteId = "activation-native-route";
constexpr std::string_view kWrongRouteId = "activation-native-wrong";
constexpr std::string_view kConcurrentRouteId = "activation-native-concurrent";
constexpr std::string_view kSnapshotA = "fnv1a64:0123456789abcdef";
constexpr std::string_view kSnapshotB = "fnv1a64:fedcba9876543210";
constexpr std::string_view kSnapshotC = "fnv1a64:1111111111111111";
const std::string kRouteFingerprint(64, 'a');
const std::string kArtifactNamespace(64, 'b');
const Json kEmptyInventory{{"root_present", false}, {"entries", Json::array()}, {"payload_bytes", 0}};

struct PreparedCandidate {
    std::uint64_t id;
    Json input;
};

struct ConcurrentOutcome {
    bool committed = false;
    bool replayed = false;
    std::string code;
    std::exception_ptr unexpected;
};

struct ContentionOutcome {
    Json result = nullptr;
    std::string code;
};

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expects_error(const char* code, const std::function<void()>& action) {
    try {
        action();
    } catch (const ServiceError& error) {
        check(error.code == code, "activation error code changed");
        return;
    }
    throw std::runtime_error("activation operation unexpectedly succeeded");
}

Principal configured_admin(const Config& config) {
    for (const auto& credential : config.credentials) {
        if (credential.principal.admin) return credential.principal;
    }
    throw std::runtime_error("configured administrator is unavailable");
}

std::string artifact_key(const Principal& principal, std::string_view idempotency_key) {
    return digest_hex(sha256(principal.id + ":" + std::string(idempotency_key)));
}

std::uint64_t preparation_id(const Json& input) {
    const auto& job = input.at("job");
    check(job.is_object() && job.at("id").is_number_integer(), "preparation inspection input is malformed");
    return job.at("id").get<std::uint64_t>();
}

Json enqueue_candidate(Store& store,
                       const Principal& admin,
                       std::string_view route,
                       std::string_view expected_snapshot_id,
                       std::string_view idempotency_key,
                       char request_hash_character) {
    const auto result = store.enqueue_preparation(
        admin, std::string(kProjectId), std::string(route), kRouteFingerprint,
        std::string(expected_snapshot_id), idempotency_key, std::string(64, request_hash_character), 64, 1,
        kArtifactNamespace, [&] { return artifact_key(admin, idempotency_key); });
    check(!result.at("replayed").get<bool>(), "new activation candidate replayed");
    return store.preparation_inspection_input(admin, preparation_id(result));
}

PreparedCandidate prepared_candidate(Store& store,
                                     const Principal& admin,
                                     std::string_view route,
                                     std::string_view expected_snapshot_id,
                                     std::string_view snapshot_id,
                                     std::string_view idempotency_key,
                                     std::string_view worker_id,
                                     char request_hash_character) {
    auto input = enqueue_candidate(store, admin, route, expected_snapshot_id, idempotency_key, request_hash_character);
    const auto id = preparation_id(input);
    const auto claimed = store.claim_preparation(std::string(worker_id), 3);
    check(!claimed.is_null() && claimed.at("id").get<std::uint64_t>() == id,
          "activation candidate claim selected another fixture row");

    // This is a Store-only ledger fixture: no candidate directory, manifest, or preflight evidence is created here.
    check(store.finish_preparation(id, std::string(worker_id), "prepared", "", std::string(snapshot_id), 1),
          "activation candidate did not reach prepared state");
    input = store.preparation_inspection_input(admin, id);
    check(input.at("job").at("state") == "prepared" &&
              input.at("job").at("artifact_snapshot_id") == std::string(snapshot_id) &&
              input.at("job").at("artifact_bytes") == 1,
          "prepared activation candidate did not retain its ledger values");
    return {id, std::move(input)};
}

Json commit(Store& store,
            const Principal& admin,
            std::string_view route,
            std::uint64_t expected_revision,
            const Json& verified_input) {
    const auto result = store.commit_activation(admin, std::string(kProjectId), std::string(route), expected_revision,
                                                verified_input);
    check(result.is_object() && result.at("replayed").is_boolean() && result.at("receipt").is_object(),
          "activation commit result is malformed");
    return result;
}

void check_receipt(const Json& result,
                   bool replayed,
                   std::uint64_t previous_revision,
                   const Json& preparation_id_value,
                   const Json& snapshot_id_value) {
    check(result.at("replayed") == replayed, "activation replay marker changed");
    const auto& receipt = result.at("receipt");
    check(receipt.at("project_id") == std::string(kProjectId) && receipt.at("route_id") == std::string(kRouteId) &&
              receipt.at("previous_revision") == previous_revision && receipt.at("revision") == previous_revision + 1 &&
              receipt.at("preparation_id") == preparation_id_value && receipt.at("snapshot_id") == snapshot_id_value &&
              receipt.at("created_at").is_string(),
          "activation receipt changed shape or identity");
}

void register_probe_project(Store& store, const Principal& admin) {
    const ProjectInput project{
        .id = std::string(kProjectId),
        .display_name = "Activation native ledger probe",
    };
    const auto registration = store.register_project(admin, project, "activation-native-register", std::string(64, 'c'));
    check(!registration.replayed && registration.project.at("id") == std::string(kProjectId),
          "activation probe project was not registered as an isolated fixture");
}

void check_runtime_cannot_update_receipts(const Config& config) {
    try {
        pqxx::connection connection{config.database.connection_string};
        pqxx::work transaction{connection};
        transaction.exec("UPDATE shin_server.activation_receipts SET revision = revision");
        transaction.abort();
    } catch (const pqxx::sql_error& error) {
        check(std::string(error.sqlstate()) == "42501", "runtime receipt write failed for an unexpected reason");
        return;
    }
    throw std::runtime_error("runtime role can update activation receipts");
}

void check_concurrent_commit(const Config& config, const Principal& admin) {
    Store setup{config.database};
    const auto first = prepared_candidate(setup, admin, kConcurrentRouteId, "", kSnapshotB,
                                          "activation-concurrent-first", "activation-worker-concurrent-first", 'd');
    const auto second = prepared_candidate(setup, admin, kConcurrentRouteId, "", kSnapshotC,
                                           "activation-concurrent-second", "activation-worker-concurrent-second", 'e');
    check(first.input.at("expected_route_revision") == 0 && second.input.at("expected_route_revision") == 0,
          "concurrent candidates did not capture the same initial revision");

    Store left{config.database};
    Store right{config.database};
    ConcurrentOutcome left_outcome;
    ConcurrentOutcome right_outcome;
    std::atomic<bool> start{false};
    const auto submit = [&](Store& store, const Json& input, ConcurrentOutcome& outcome) {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            const auto result = commit(store, admin, kConcurrentRouteId, 0, input);
            outcome.committed = !result.at("replayed").get<bool>();
            outcome.replayed = result.at("replayed").get<bool>();
        } catch (const ServiceError& error) {
            outcome.code = error.code;
        } catch (...) {
            outcome.unexpected = std::current_exception();
        }
    };

    std::thread left_thread(submit, std::ref(left), std::cref(first.input), std::ref(left_outcome));
    std::thread right_thread(submit, std::ref(right), std::cref(second.input), std::ref(right_outcome));
    start.store(true, std::memory_order_release);
    left_thread.join();
    right_thread.join();
    if (left_outcome.unexpected) std::rethrow_exception(left_outcome.unexpected);
    if (right_outcome.unexpected) std::rethrow_exception(right_outcome.unexpected);

    check(static_cast<int>(left_outcome.committed) + static_cast<int>(right_outcome.committed) == 1 &&
              static_cast<int>(left_outcome.code == "activation-conflict") +
                  static_cast<int>(right_outcome.code == "activation-conflict") == 1 &&
              !left_outcome.replayed && !right_outcome.replayed,
          "different concurrent activation targets did not serialize through one compare-and-swap transition");
    const auto active = setup.active_route(admin, std::string(kProjectId), std::string(kConcurrentRouteId));
    check(active.at("revision") == 1 &&
              (active.at("preparation_id") == first.id || active.at("preparation_id") == second.id),
          "concurrent activation did not retain exactly one active generation");
}

int wait_for_blocked_backend(pqxx::connection& observer,
                             int blocker,
                             std::string_view wait_event,
                             std::string_view query_pattern,
                             const std::future<ContentionOutcome>& operation) {
    // The deadline only bounds failure. Progress requires an observed PostgreSQL lock edge.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pqxx::read_transaction transaction{observer};
        const auto waiting = transaction.exec(
            "SELECT pid FROM pg_stat_activity WHERE datname=current_database() AND usename=current_user "
            "AND wait_event_type='Lock' AND wait_event=$2 AND query LIKE $3 "
            "AND $1=ANY(pg_blocking_pids(pid))",
            pqxx::params{transaction, blocker, std::string(wait_event), std::string(query_pattern)});
        transaction.commit();
        check(waiting.size() <= 1, "contention barrier matched unrelated database sessions");
        if (!waiting.empty()) return waiting[0][0].as<int>();
        check(operation.wait_for(std::chrono::seconds(0)) != std::future_status::ready,
              "contention operation finished before reaching its database barrier");
        std::this_thread::yield();
    }
    throw std::runtime_error("contention operation did not reach its database barrier");
}

void check_contention_ledger(pqxx::connection& observer,
                             std::uint64_t id,
                             bool active,
                             std::string_view retention_state,
                             int receipts) {
    pqxx::read_transaction transaction{observer};
    transaction.exec("SELECT set_config('shin.preparation_ops','1',true),set_config('shin.audit_ops','1',true),set_config('shin.route_refs','1',true)");
    const auto rows = transaction.exec(
        "SELECT p.state, "
        "(SELECT count(*) FROM shin_server.active_routes a WHERE a.preparation_id=p.id) AS active_count, "
        "(SELECT r.state FROM shin_server.preparation_retention r WHERE r.preparation_id=p.id) AS retention_state, "
        "(SELECT count(*) FROM shin_server.activation_receipts a WHERE a.preparation_id=p.id) AS receipt_count, "
        "(SELECT count(*) FROM shin_server.audit_events e WHERE e.preparation_id=p.id "
        "AND e.action='generation.activated') AS activation_audits, "
        "(SELECT count(*) FROM shin_server.audit_events e WHERE e.preparation_id=p.id "
        "AND e.action='preparation.retention-requested') AS intent_audits, "
        "(SELECT count(*) FROM shin_server.audit_events e WHERE e.preparation_id=p.id "
        "AND e.action='preparation.retention-released') AS release_audits "
        "FROM shin_server.preparations p WHERE p.id=$1",
        pqxx::params{transaction, id});
    check(rows.size() == 1, "contention candidate disappeared");
    const auto& row = rows[0];
    const auto actual_retention = row["retention_state"].is_null() ? "" : row["retention_state"].as<std::string>();
    check(row["active_count"].as<int>() == 0 || actual_retention.empty(),
          "candidate is simultaneously active and retained for intent or release");
    check(row["state"].as<std::string>() == "prepared" &&
              row["active_count"].as<int>() == static_cast<int>(active) && actual_retention == retention_state &&
              row["receipt_count"].as<int>() == receipts && row["activation_audits"].as<int>() == receipts &&
              row["intent_audits"].as<int>() == static_cast<int>(!retention_state.empty()) &&
              row["release_audits"].as<int>() == static_cast<int>(retention_state == "released"),
          "contention changed the expected candidate, receipt, or audit ledger");
    transaction.commit();
}

void check_activation_retention_contention(const Config& config, const Principal& admin, bool activation_first) {
    const std::string route = activation_first ? "activation-wins-retention" : "retention-wins-activation";
    const auto plan_id = std::string(64, activation_first ? '6' : '7');
    Store setup{config.database};
    const auto candidate = prepared_candidate(setup, admin, route, "", kSnapshotB, route, route, '8');
    check(candidate.input.at("expected_route_revision") == 0,
          "contention candidate did not bind an unpublished route");

    auto database = config.database;
    database.pool_size = 1;
    Store activation{database};
    Store retention{database};
    // Warm both independent Store connections before holding the bounded production audit lock.
    (void)activation.active_route(admin, std::string(kProjectId), route);
    (void)retention.active_route(admin, std::string(kProjectId), route);
    pqxx::connection controller{database.connection_string};
    pqxx::connection observer{database.connection_string};
    check_contention_ledger(observer, candidate.id, false, "", 0);

    const auto activate = [&]() -> ContentionOutcome {
        try {
            return {commit(activation, admin, route, 0, candidate.input), ""};
        } catch (const ServiceError& error) {
            return {nullptr, error.code};
        }
    };
    const auto retain = [&]() -> ContentionOutcome {
        try {
            return {retention.begin_retention(admin, candidate.id, candidate.input, plan_id, kEmptyInventory), ""};
        } catch (const ServiceError& error) {
            return {nullptr, error.code};
        }
    };

    pqxx::work barrier{controller};
    const int controller_pid = barrier.exec("SELECT pg_backend_pid()")[0][0].as<int>();
    barrier.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1'))");
    std::future<ContentionOutcome> first;
    std::future<ContentionOutcome> second;
    try {
        first = std::async(std::launch::async, [&] { return activation_first ? activate() : retain(); });
        // Both writers reach this existing audit fence only after locking the preparation row.
        const int first_pid = wait_for_blocked_backend(observer, controller_pid, "advisory",
            "SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1'))%", first);
        second = std::async(std::launch::async, [&] { return activation_first ? retain() : activate(); });
        const int second_pid = wait_for_blocked_backend(observer, first_pid, "transactionid",
            "SELECT % FROM shin_server.preparations WHERE id%FOR UPDATE", second);
        check(first_pid != second_pid && first_pid != controller_pid && second_pid != controller_pid,
              "contention operations did not use independent database connections");
        check(first.wait_for(std::chrono::seconds(0)) != std::future_status::ready,
              "first writer escaped the audit barrier before the second acquired its wait edge");
        check_contention_ledger(observer, candidate.id, false, "", 0);
        barrier.commit();
    } catch (...) {
        // Release before future destruction joins workers, including barrier/assertion failures.
        barrier.abort();
        throw;
    }
    const auto first_result = first.get();
    const auto second_result = second.get();
    const auto& activation_result = activation_first ? first_result : second_result;
    const auto& retention_result = activation_first ? second_result : first_result;
    const auto active = setup.active_route(admin, std::string(kProjectId), route);

    if (activation_first) {
        check(activation_result.code.empty() && activation_result.result.at("replayed") == false &&
                  activation_result.result.at("receipt").at("preparation_id") == candidate.id &&
                  activation_result.result.at("receipt").at("previous_revision") == 0 &&
                  activation_result.result.at("receipt").at("revision") == 1 &&
                  retention_result.result.is_null() && retention_result.code == "candidate-active",
              "activation-first contention did not commit activation and reject retention");
        check(active.at("revision") == 1 && active.at("preparation_id") == candidate.id &&
                  active.at("snapshot_id") == std::string(kSnapshotB),
              "activation-first contention selected an unexpected active generation");
        check_contention_ledger(observer, candidate.id, true, "", 1);
        expects_error("candidate-active", [&] {
            (void)retention.finish_retention(admin, candidate.id, plan_id, kArtifactNamespace,
                                              candidate.input.at("artifact_key").get<std::string>());
        });
        check_contention_ledger(observer, candidate.id, true, "", 1);
    } else {
        check(retention_result.code.empty() && retention_result.result.at("state") == "intent" &&
                  retention_result.result.at("plan_id") == plan_id && activation_result.result.is_null() &&
                  activation_result.code == "candidate-retention-pending",
              "retention-first contention did not commit intent and reject activation");
        check(active.at("revision") == 0 && active.at("preparation_id").is_null() && active.at("snapshot_id").is_null(),
              "retention-first contention changed the unpublished route");
        check_contention_ledger(observer, candidate.id, false, "intent", 0);
        const auto released = retention.finish_retention(admin, candidate.id, plan_id, kArtifactNamespace,
                                                          candidate.input.at("artifact_key").get<std::string>());
        check(released.at("state") == "released" && released.at("plan_id") == plan_id,
              "retention-first candidate did not finish release");
        expects_error("candidate-retention-pending", [&] {
            (void)commit(activation, admin, route, 0, candidate.input);
        });
        check_contention_ledger(observer, candidate.id, false, "released", 0);
    }
}

void run(const fs::path& config_path) {
    check(config_path.is_absolute(), "configuration path must be absolute");
    const Config config = load_config(config_path);
    check(config.database.expected_database == "shin_server_dev", "refusing to run outside the dedicated Shin Server database");
    check(config.database.runtime_role == "shin_runtime", "activation probe requires the dedicated runtime configuration");
    const Principal admin = configured_admin(config);
    Store store{config.database};
    register_probe_project(store, admin);

    const auto initial = store.active_route(admin, std::string(kProjectId), std::string(kRouteId));
    check(initial.at("revision") == 0 && initial.at("preparation_id").is_null() && initial.at("snapshot_id").is_null(),
          "new route did not begin as an unpublished revision-zero generation");
    check(store.activation_replay(admin, std::string(kProjectId), std::string(kRouteId), 0, Json(nullptr)).is_null(),
          "unpublished route unexpectedly has an activation receipt");

    const auto first = prepared_candidate(store, admin, kRouteId, "", kSnapshotA,
                                           "activation-first", "activation-worker-first", 'f');
    check(first.input.at("expected_route_revision") == 0, "first candidate did not bind revision zero");
    const auto first_commit = commit(store, admin, kRouteId, 0, first.input);
    check_receipt(first_commit, false, 0, first.id, std::string(kSnapshotA));
    const auto first_active = store.active_route(admin, std::string(kProjectId), std::string(kRouteId));
    check(first_active.at("revision") == 1 && first_active.at("preparation_id") == first.id &&
              first_active.at("snapshot_id") == std::string(kSnapshotA) &&
              first_active.at("artifact_namespace") == kArtifactNamespace &&
              first_active.at("artifact_key") == first.input.at("artifact_key") && first_active.at("artifact_bytes") == 1 &&
              store.preparation_is_active(admin, first.id),
          "first activation did not select its private prepared generation from the database");

    const auto first_lookup = store.activation_replay(admin, std::string(kProjectId), std::string(kRouteId), 0,
                                                      first.input.at("job").at("id"));
    check(first_lookup.at("replayed") == true && first_lookup.at("receipt") == first_commit.at("receipt"),
          "historical activation receipt was not stable");
    const auto first_replay = commit(store, admin, kRouteId, 0, first.input);
    check(first_replay.at("replayed") == true && first_replay.at("receipt") == first_commit.at("receipt") &&
              store.active_route(admin, std::string(kProjectId), std::string(kRouteId)) == first_active,
          "same activation request changed the active generation or receipt");

    const auto second = prepared_candidate(store, admin, kRouteId, kSnapshotA, kSnapshotA,
                                            "activation-second", "activation-worker-second", '1');
    check(second.input.at("expected_route_revision") == 1 &&
              second.input.at("job").at("artifact_snapshot_id") == std::string(kSnapshotA),
          "same-content successor did not bind the first activation revision");
    expects_error("activation-conflict", [&] {
        (void)store.activation_replay(admin, std::string(kProjectId), std::string(kRouteId), 0,
                                      second.input.at("job").at("id"));
    });

    const auto wrong_route = prepared_candidate(store, admin, kWrongRouteId, "", kSnapshotB,
                                                 "activation-wrong-route", "activation-worker-wrong-route", '2');
    const auto retained = prepared_candidate(store, admin, kRouteId, kSnapshotA, kSnapshotB,
                                              "activation-retained", "activation-worker-retained", '3');
    const auto retained_plan_id = std::string(64, 'd');
    const auto retained_intent = store.begin_retention(admin, retained.id, retained.input, retained_plan_id, kEmptyInventory);
    check(retained_intent.at("state") == "intent", "retention intent fixture was not created");
    const auto queued = enqueue_candidate(store, admin, kRouteId, kSnapshotA, "activation-queued", '4');
    check(queued.at("job").at("state") == "queued", "nonprepared activation fixture is not queued");

    expects_error("activation-conflict", [&] { (void)commit(store, admin, kRouteId, 1, wrong_route.input); });
    expects_error("activation-conflict", [&] { (void)commit(store, admin, kRouteId, 1, queued); });
    expects_error("candidate-retention-pending", [&] { (void)commit(store, admin, kRouteId, 1, retained.input); });
    check(store.active_route(admin, std::string(kProjectId), std::string(kRouteId)) == first_active,
          "rejected activation candidate changed the active route");
    const auto cancelled = store.cancel_preparation(admin, preparation_id(queued));
    check(cancelled.at("state") == "cancelled" && cancelled.at("reason_code") == "cancelled",
          "nonprepared activation fixture was not cancelled after its rejection check");

    const auto second_commit = commit(store, admin, kRouteId, 1, second.input);
    check_receipt(second_commit, false, 1, second.id, std::string(kSnapshotA));
    const auto second_active = store.active_route(admin, std::string(kProjectId), std::string(kRouteId));
    check(second_active.at("revision") == 2 && second_active.at("preparation_id") == second.id &&
              store.preparation_is_active(admin, second.id) && !store.preparation_is_active(admin, first.id),
          "same-content successor did not advance the active route revision");

    expects_error("candidate-active", [&] {
        (void)store.begin_retention(admin, second.id, second.input, std::string(64, 'e'), kEmptyInventory);
    });
    expects_error("candidate-active", [&] {
        (void)store.finish_retention(admin, second.id, std::string(64, 'e'),
                                     second.input.at("artifact_namespace").get<std::string>(),
                                     second.input.at("artifact_key").get<std::string>());
    });

    const auto deactivation = commit(store, admin, kRouteId, 2, Json(nullptr));
    check_receipt(deactivation, false, 2, Json(nullptr), Json(nullptr));
    const auto inactive = store.active_route(admin, std::string(kProjectId), std::string(kRouteId));
    check(inactive.at("revision") == 3 && inactive.at("preparation_id").is_null() && inactive.at("snapshot_id").is_null() &&
              !store.preparation_is_active(admin, second.id),
          "explicit deactivation did not advance the route revision or clear the active generation");

    const auto aba_candidate = prepared_candidate(store, admin, kRouteId, "", kSnapshotC,
                                                   "activation-aba", "activation-worker-aba", '5');
    check(aba_candidate.input.at("expected_route_revision") == 3,
          "candidate after deactivation did not bind the new inactive revision");
    // The route is unpublished again, but revision zero is not reusable after the activation/deactivation cycle.
    expects_error("activation-conflict", [&] { (void)commit(store, admin, kRouteId, 0, aba_candidate.input); });

    Principal reader = admin;
    reader.admin = false;
    reader.project_ids = {"outside-project"};
    expects_error("project-not-found", [&] {
        (void)store.active_route(reader, std::string(kProjectId), std::string(kRouteId));
    });

    check_runtime_cannot_update_receipts(config);
    check_concurrent_commit(config, admin);
    check_activation_retention_contention(config, admin, true);
    check_activation_retention_contention(config, admin, false);
}

void report(bool ok) {
    if (ok) {
        std::cout << "{\"ok\":true,\"checks\":[\"initial-unpublished\",\"activation-replay\","
                     "\"compare-and-swap\",\"candidate-rejections\",\"active-retention-protection\","
                     "\"deactivation-and-aba\",\"reader-scope\",\"receipt-write-denied\","
                     "\"concurrent-compare-and-swap\",\"activation-before-retention-row-lock\","
                     "\"retention-before-activation-row-lock\"]}\n";
    } else {
        std::cout << "{\"ok\":false,\"checks\":[]}\n";
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
    } catch (...) {
        report(false);
        return 1;
    }
}
