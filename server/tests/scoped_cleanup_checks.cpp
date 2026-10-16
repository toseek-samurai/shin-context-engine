#include "scoped_cleanup_checks.h"
#include "shin/server/preparation_service.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/retention_files.h"
#include "shin/server/route_files.h"

namespace {
using namespace shin::server;
unsigned checks = 0;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class F> void denied(F operation, int status, const char* code) {
    try { operation(); }
    catch (const ServiceError& error) {
        if (error.status != status || error.code != code)
            throw std::runtime_error("unexpected cleanup denial: " + error.code + " after check " + std::to_string(checks));
        ++checks;
        return;
    }
    throw std::runtime_error("cleanup unexpectedly admitted");
}
}

unsigned check_scoped_cleanup(const shin::server::Config& config, std::vector<std::uint64_t>& jobs) {
    checks = 0;
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "cleanup fixture requires dedicated stand");
    Store store(config.database);
    RouteCatalog catalog(config);
    PreparationService service(config, catalog, store);
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    check(admin.admin, "cleanup maintenance identity missing");
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}};
    const WorkspaceReadRequest own{"scope-a", "primary"}, other{"scope-b", "primary"};
    auto a = store.capture_workspace_write(alice, "visible", own);
    const auto b = store.capture_workspace_write(bob, "other", other);
    const auto enqueue = [&](const char* key) {
        const auto base = store.scoped_active_route(alice, catalog.find("visible", "visible-retrieval").route, own);
        const auto result = service.enqueue(alice, "visible", "visible-retrieval", {{"expected_snapshot_id", base.at("snapshot_id")}}, key, own);
        const auto id = result.at("job").at("id").get<std::uint64_t>();
        jobs.push_back(id);
        return id;
    };
    const auto queued = enqueue("scoped-cleanup-queued");
    denied([&] { store.cancel_preparation(alice, queued); }, 403, "forbidden");
    denied([&] { store.cancel_preparation(bob, queued, b); }, 404, "preparation-not-found");
    denied([&] { service.retention_plan(bob, queued, b); }, 404, "preparation-not-found");
    const Json fake_plan{{"expected_plan_id", std::string(64, 'a')}};
    denied([&] { service.apply_retention(bob, queued, fake_plan, b); }, 404, "preparation-not-found");
    check(service.retention_plan(alice, queued, a).at("reason_codes") == Json::array({"retention-job-active"}), "queued cleanup allowed");
    check(store.cancel_preparation(alice, queued, a).at("state") == "cancelled", "owner cancellation failed");
    check(store.cancel_preparation(alice, queued, a).at("state") == "cancelled", "owner cancellation replay failed");
    const auto plan = service.retention_plan(alice, queued, a);
    check(plan.at("can_apply") == true && plan.at("artifact_bytes") == 0, "never-claimed job not reclaimable");
    const Json request{{"expected_plan_id", plan.at("plan_id")}};
    denied([&] { service.apply_retention(alice, queued, request); }, 403, "forbidden");

    // Commit only the real empty-tree intent, then lose authority before completion.
    // No fabricated prepared artifact and no test-only production callback.
    const auto input = store.preparation_cleanup_input(alice, queued, a);
    const auto key = input.at("artifact_key").get<std::string>();
    const auto space = input.at("artifact_namespace").get<std::string>();
    const auto inventory = inspect_retention_tree(config.preparations.state_dir / candidate_artifact_name(queued, key),
        input.at("job").at("reserved_artifact_bytes").get<std::uint64_t>());
    const auto plan_id = plan.at("plan_id").get<std::string>();
    denied([&] { store.begin_retention(bob, queued, input, plan_id, inventory, b); }, 404, "preparation-not-found");
    denied([&] { store.finish_retention(bob, queued, plan_id, space, key, b); }, 404, "preparation-not-found");
    check(store.begin_retention(alice, queued, input, plan_id, inventory, a).at("state") == "intent", "owner intent failed");
    const auto account_revision = a.admission.at("authorization").at("account_revision").get<std::uint64_t>();
    store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", account_revision}});
    denied([&] { service.retention_plan(alice, queued, a); }, 404, "identity-not-found");
    denied([&] { store.cancel_preparation(alice, queued, a); }, 404, "identity-not-found");
    denied([&] { store.begin_retention(alice, queued, input, plan_id, inventory, a); }, 404, "identity-not-found");
    denied([&] { store.finish_retention(alice, queued, plan_id, space, key, a); }, 404, "identity-not-found");
    denied([&] { service.apply_retention(alice, queued, request, a); }, 404, "identity-not-found");
    store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", account_revision + 1}});
    denied([&] { service.retention_plan(alice, queued, a); }, 409, "workspace-write-scope-changed");
    denied([&] { store.cancel_preparation(alice, queued, a); }, 409, "workspace-write-scope-changed");
    denied([&] { store.begin_retention(alice, queued, input, plan_id, inventory, a); }, 409, "workspace-write-scope-changed");
    denied([&] { store.finish_retention(alice, queued, plan_id, space, key, a); }, 409, "workspace-write-scope-changed");
    denied([&] { service.apply_retention(alice, queued, request, a); }, 409, "workspace-write-scope-changed");
    a = store.capture_workspace_write(alice, "visible", own);
    denied([&] { service.inspect(alice, queued, a); }, 404, "preparation-not-found");
    {
        Store reopened(config.database);
        PreparationService recovered(config, catalog, reopened);
        check(recovered.retention_plan(alice, queued, a).at("state") == "intent", "intent did not survive regrant/reopen");
        const auto result = recovered.apply_retention(alice, queued, request, a);
        check(result.at("state") == "released" && result.at("reservation_held") == false, "fresh owner did not recover cleanup");
        check(recovered.apply_retention(alice, queued, request, a).at("mutation_performed") == false, "released cleanup replay mutated");
    }
    denied([&] { service.apply_retention(bob, queued, request, b); }, 404, "preparation-not-found");

    const auto running = enqueue("scoped-cleanup-running");
    const std::string worker = "scoped-cleanup-worker";
    const auto claimed = store.claim_preparation(worker, 30);
    check(claimed.at("id") == running, "wrong cleanup job claimed");
    {
        const CandidateArtifactLock lifetime(config.preparations.state_dir, route_directory_identity(config.preparations.state_dir),
            running, claimed.at("artifact_key").get<std::string>(), CandidateArtifactLock::Mode::create_writer);
        const auto result = store.cancel_preparation(alice, running, a);
        check(result.at("state") == "running" && result.at("cancel_requested") == true, "running cancellation ignored");
        check(store.cancel_preparation(alice, running, a).at("cancel_requested") == true, "running cancellation replay lost");
        denied([&] { service.apply_retention(alice, running, fake_plan, a); }, 409, "retention-job-active");
        check(store.finish_preparation(running, worker, "cancelled", "cancelled", "", 0), "worker cancellation did not finish");
    }

    // The original probe's real Core artifacts include jobs with obsolete admission.
    // Cleanup is allowed under fresh owner authority; inspection/promotion is not.
    for (const auto id : jobs) {
        const auto job = store.get_preparation(admin, id);
        const bool is_alice = job.at("project_id") == "visible";
        const auto& actor = is_alice ? alice : bob;
        const auto& scope = is_alice ? a : b;
        const auto own_plan = service.retention_plan(actor, id, scope);
        if (own_plan.at("state") == "released") continue;
        check(own_plan.at("can_apply") == true, "owner cannot clean obsolete candidate");
        for (const auto* field : {"root", "artifact_key", "artifact_namespace", "workspace_admission", "worker_id"})
            check(!own_plan.contains(field), "retention plan exposed private identity");
        const auto released = service.apply_retention(actor, id, {{"expected_plan_id", own_plan.at("plan_id")}}, scope);
        check(released.at("state") == "released" && released.at("reservation_held") == false, "owner cleanup failed");
        check(service.apply_retention(actor, id, {{"expected_plan_id", own_plan.at("plan_id")}}, scope).at("mutation_performed") == false,
              "owner cleanup replay failed");
    }
    return checks;
}
