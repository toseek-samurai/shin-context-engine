#include "shin/server/preparation_service.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/route_files.h"
#include "scoped_promotion_checks.h"
#include "scoped_request_key_checks.h"
#include "scoped_cleanup_checks.h"
#include "personal_route_checks.h"
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace shin::server;
unsigned checks = 0;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class F> void denied(F operation, int status, const char* code) {
    try { operation(); }
    catch (const ServiceError& error) { check(error.status == status && error.code == code, "unexpected job denial"); return; }
    throw std::runtime_error("job unexpectedly admitted");
}
void run(const std::filesystem::path& path) {
    auto config = load_config(path);
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "dedicated runtime stand required");
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    check(admin.admin, "maintenance identity missing");
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}};
    // Native-only fixture principals, no transport token or production grant.
    config.credentials.push_back({alice, {}});
    config.credentials.push_back({bob, {}});
    config.preparations.enabled = true;
    Store store(config.database);
    RouteCatalog catalog(config);
    const WorkspaceReadRequest own{"scope-a", "primary"}, shared{"scope-b", "primary"};
    const auto base = store.scoped_active_route(alice, catalog.find("visible", "visible-retrieval").route, own);
    const auto other_base = store.scoped_active_route(bob, catalog.find("other", "other-retrieval").route, shared);
    const Json request{{"expected_snapshot_id", base.at("snapshot_id")}};
    const std::string worker = "scoped-fixture-worker";
    std::vector<std::uint64_t> jobs;
    const auto cleanup = [&] {
        PreparationService service(config, catalog, store);
        for (const auto id : jobs) {
            store.cancel_preparation(admin, id);
            (void)store.finish_preparation(id, worker, "failed", "fixture-cleanup", "", 0);
            const auto plan = service.retention_plan(admin, id);
            if (plan.at("state") == "released") continue;
            check(plan.at("can_apply") == true, "fixture artifact not reclaimable");
            check(service.apply_retention(admin, id, {{"expected_plan_id", plan.at("plan_id")}}).at("state") == "released",
                  "fixture artifact cleanup failed");
        }
    };
    try {
        std::uint64_t live_a = 0, live_b = 0;
        {
            PreparationService service(config, catalog, store);
            denied([&] { service.enqueue(alice, "other", "other-retrieval", {{"expected_snapshot_id", other_base.at("snapshot_id")}}, "reader-denied", shared); },
                   404, "identity-not-found");
            denied([&] { service.enqueue(bob, "visible", "visible-retrieval", request, "private-denied", own); }, 404, "identity-not-found");
            denied([&] { service.enqueue(alice, "visible", "visible-retrieval", request, "tenant-denied", shared); }, 404, "identity-not-found");
            const auto enqueue = [&](const std::string& key, const Json& body = Json()) {
                const auto result = service.enqueue(alice, "visible", "visible-retrieval", body.is_null() ? request : body, key, own);
                check(result.at("replayed") == false, "new job replayed");
                const auto id = result.at("job").at("id").get<std::uint64_t>();
                jobs.push_back(id);
                return id;
            };
            const auto queued = enqueue("scoped-queued");
            check(service.enqueue(alice, "visible", "visible-retrieval", request, "scoped-queued", own).at("replayed") == true, "job replay failed");
            store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", 5}});
            denied([&] { service.enqueue(alice, "visible", "visible-retrieval", request, "scoped-queued", own); }, 404, "identity-not-found");
            check(store.claim_preparation(worker, 30).is_null(), "revoked queued job was delivered");
            store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", 6}});
            const auto rejected = store.scoped_preparation(alice, "visible", queued, own);
            check(rejected.at("state") == "failed" && rejected.at("reason_code") == "preparation-scope-changed", "revoked queue did not terminate");
            denied([&] { service.enqueue(alice, "visible", "visible-retrieval", request, "scoped-queued", own); }, 409, "idempotency-conflict");

            const auto running = enqueue("scoped-running");
            const auto job = store.claim_preparation(worker, 30);
            check(job.at("id") == running && !job.at("workspace_admission").is_null(), "durable job scope missing");
            {
                const CandidateArtifactLock lifetime(config.preparations.state_dir, route_directory_identity(config.preparations.state_dir),
                    running, job.at("artifact_key").get<std::string>(), CandidateArtifactLock::Mode::create_writer);
                Store reopened(config.database);
                check(reopened.renew_preparation(running, worker, 30), "reopened store lost valid lease");
                store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", 7}});
                store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", 8}});
                check(!reopened.renew_preparation(running, worker, 30), "regrant revived obsolete lease");
                check(reopened.finish_preparation(running, worker, "prepared", "", base.at("snapshot_id"), 1), "revoked completion did not terminate");
            }
            const auto failed = store.scoped_preparation(alice, "visible", running, own);
            check(failed.at("state") == "failed" && failed.at("artifact_snapshot_id").is_null() &&
                  failed.at("reason_code") == "preparation-scope-changed", "revoked completion published artifact metadata");

            const auto changed = enqueue("scoped-base", {{"expected_snapshot_id", "fnv1a64:0000000000000000"}});
            const auto stale = store.claim_preparation(worker, 30);
            check(stale.at("id") == changed, "wrong scoped base job");
            {
                const CandidateArtifactLock lifetime(config.preparations.state_dir, route_directory_identity(config.preparations.state_dir),
                    changed, stale.at("artifact_key").get<std::string>(), CandidateArtifactLock::Mode::create_writer);
                check(store.finish_preparation(changed, worker, "prepared", "", base.at("snapshot_id"), 1), "stale completion did not terminate");
            }
            check(store.scoped_preparation(alice, "visible", changed, own).at("reason_code") == "preparation-base-changed", "stale base accepted");
            live_a = enqueue("scoped-success");
            const auto shared_job = service.enqueue(bob, "other", "other-retrieval", {{"expected_snapshot_id", other_base.at("snapshot_id")}}, "scoped-success", shared);
            live_b = shared_job.at("job").at("id").get<std::uint64_t>();
            jobs.push_back(live_b);
            check(shared_job.at("replayed") == false && live_a != live_b, "job identity crossed owners");
            denied([&] { store.scoped_preparation(alice, "other", live_b, shared); }, 404, "identity-not-found");
        }
        // A new worker and connection pool must honor the persisted admission.
        {
            Store reopened(config.database);
            PreparationService worker_service(config, catalog, reopened);
            worker_service.start();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (true) {
                const auto a = store.scoped_preparation(alice, "visible", live_a, own);
                const auto b = store.scoped_preparation(bob, "other", live_b, shared);
                if (a.at("state") == "failed" || b.at("state") == "failed") throw std::runtime_error("native scoped worker failed");
                if (a.at("state") == "prepared" && b.at("state") == "prepared") {
                    check(a.at("artifact_bytes") > 0 && b.at("artifact_bytes") > 0, "worker generated no artifact");
                    for (const auto* field : {"workspace_admission", "worker_id", "artifact_key", "root", "request_hash"})
                        check(!a.contains(field) && !b.contains(field), "job metadata exposed private fields");
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("scoped worker deadline exceeded");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        check(store.scoped_active_route(alice, catalog.find("visible", "visible-retrieval").route, own) == base, "preparation activated source implicitly");
        checks += check_scoped_promotions(config, live_a, live_b);
        checks += check_scoped_cleanup(config, jobs);
        denied([&] { store.identity_request_admission(alice, false); }, 403, "identity-data-plane-not-enabled");
    } catch (...) { cleanup(); throw; }
    cleanup();
    checks += check_personal_routes(config);
    checks += check_scoped_request_keys(config);
    std::cout << Json{{"ok", true}, {"checks", checks}, {"scoped_jobs", true},
                     {"scoped_promotion", true}, {"scoped_retention", true}, {"scoped_request_keys", true},
                     {"scoped_cleanup", true}, {"personal_routes_without_publications", true}}.dump() << '\n';
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { run(argv[1]); }
    catch (const shin::server::ServiceError& error) { std::cerr << error.code << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
