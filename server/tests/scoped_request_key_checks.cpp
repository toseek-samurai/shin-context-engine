#include "scoped_request_key_checks.h"
#include "shin/server/store.h"
#include <array>
#include <future>
#include <pqxx/pqxx>

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
        check(error.status == status && error.code == code, "unexpected request-key denial");
        return;
    }
    throw std::runtime_error("request-key operation unexpectedly admitted");
}
std::uint64_t job_id(const Json& result) { return result.at("job").at("id").get<std::uint64_t>(); }
std::uint64_t proposal_id(const Json& result) { return result.at("proposal").at("proposal").at("id").get<std::uint64_t>(); }
}

unsigned check_scoped_request_keys(const shin::server::Config& config) {
    checks = 0;
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "request-key fixture requires dedicated stand");
    Store store(config.database);
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    check(admin.admin, "request-key maintenance identity missing");
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}};
    const std::string hash(64, 'a'), fingerprint(64, 'b'), artifact_namespace(64, 'c');
    for (const auto* project : {"keys-project-a", "keys-project-b"}) {
        store.register_project(admin, {project, "Same display name"}, project, hash);
        store.identity_bind_project(admin, "scope-a", {{"project_id", project}});
    }
    std::array<WorkspaceWriteScope, 3> scopes;
    const std::array<std::pair<std::string, std::string>, 3> coordinates{{
        {"keys-project-a", "primary"}, {"keys-project-a", "secondary"}, {"keys-project-b", "primary"}}};
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        const auto& [project, workspace] = coordinates[i];
        const auto marker = project + "-" + workspace;
        store.register_workspace(admin, project, {workspace, "Same checkout name", config.database.source_host_id,
            "/fixture/" + marker, marker}, marker, hash);
        store.identity_bind_workspace(admin, "scope-a", project, {{"workspace_id", workspace}, {"owner_id", "alice"}});
        scopes[i] = store.capture_workspace_write(alice, project, {"scope-a", workspace});
        scopes[i].route_id = "key-route";
    }
    const auto enqueue = [&](const Principal& actor, const WorkspaceWriteScope& scope, const std::string& key,
                             const std::string& request_hash, bool legacy = false) {
        return store.enqueue_preparation(actor, scope.project_id, scope.route_id, fingerprint, "", key, request_hash, 64, 1,
            artifact_namespace, [&] { return digest_hex(sha256(actor.id + scope.project_id + scope.workspace.workspace_id + key +
                (legacy ? "legacy" : "scoped"))); }, legacy ? std::nullopt : std::optional(scope.workspace),
            legacy ? Json() : scope.admission);
    };
    // Duplicate delivery races in two workspaces of one project, not two users.
    std::array<std::future<Json>, 4> pending;
    for (std::size_t i = 0; i < pending.size(); ++i)
        pending[i] = std::async(std::launch::async, [&, i] { return enqueue(alice, scopes[i % 2], "same-key", hash); });
    std::array<std::uint64_t, 3> jobs{};
    unsigned created = 0;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto result = pending[i].get();
        if (!result.at("replayed").get<bool>()) ++created;
        if (i < 2) jobs[i] = job_id(result);
        else check(job_id(result) == jobs[i % 2], "concurrent workspace replay changed job");
    }
    check(created == 2 && jobs[0] != jobs[1], "same-owner workspace job collision");
    denied([&] { store.cancel_preparation(alice, jobs[0], scopes[1]); }, 404, "preparation-not-found");
    denied([&] { store.preparation_cleanup_input(alice, jobs[0], scopes[1]); }, 404, "preparation-not-found");
    const auto third = enqueue(alice, scopes[2], "same-key", hash);
    jobs[2] = job_id(third);
    check(third.at("replayed") == false && jobs[2] != jobs[0] && jobs[2] != jobs[1], "same-owner project job collision");
    bool allocated = false;
    denied([&] {
        store.enqueue_preparation(alice, scopes[0].project_id, scopes[0].route_id, fingerprint, "", "quota-cross-scope", hash,
            2, 1, artifact_namespace, [&] { allocated = true; return std::string(64, 'e'); }, scopes[0].workspace, scopes[0].admission);
    }, 409, "preparation-capacity");
    check(!allocated && store.scoped_preparations(alice, scopes[0].project_id, 0, 100, scopes[0].workspace).at("items").size() == 1,
          "RLS reduced the global quota to visible jobs or persisted a rejected job");
    Store reopened(config.database);
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        const auto& scope = scopes[i];
        check(enqueue(alice, scope, "same-key", hash).at("replayed") == true, "scoped job replay missing");
        check(reopened.scoped_preparation(alice, scope.project_id, jobs[i], scope.workspace).at("id") == jobs[i], "persisted job scope lost");
        denied([&] { enqueue(alice, scope, "same-key", std::string(64, 'd')); }, 409, "idempotency-conflict");
        denied([&] { enqueue(bob, scope, "same-key", hash); }, 404, "identity-not-found");
    }
    auto maintenance = store.capture_workspace_write(admin, scopes[0].project_id, scopes[0].workspace);
    maintenance.route_id = "key-route";
    const auto legacy = enqueue(admin, maintenance, "legacy-overlap", hash, true);
    const auto scoped = enqueue(admin, maintenance, "legacy-overlap", hash);
    check(job_id(legacy) != job_id(scoped), "legacy job collided with scoped request");
    denied([&] { store.preparation_cleanup_input(alice, job_id(legacy), scopes[0]); }, 404, "preparation-not-found");
    check(enqueue(admin, maintenance, "legacy-overlap", hash, true).at("replayed") == true, "legacy job replay lost");
    check(enqueue(admin, maintenance, "legacy-overlap", hash).at("replayed") == true, "maintenance scoped replay lost");

    const auto more = enqueue(alice, scopes[0], "inventory-next", hash);
    const auto inventory = [&](const Principal& actor, std::size_t index, std::uint64_t after, std::size_t limit) {
        return reopened.scoped_preparations(actor, scopes[index].project_id, after, limit, scopes[index].workspace);
    };
    const auto first = inventory(alice, 0, 0, 1);
    check(first.at("items").size() == 1 && first.at("items")[0].at("id") == jobs[0] && first.at("next_cursor") == jobs[0],
          "scoped job page mixed workspace or principal");
    const auto second = inventory(alice, 0, jobs[0], 1);
    check(second.at("items").size() == 1 && second.at("items")[0].at("id") == job_id(more) && second.at("next_cursor").is_null(),
          "hidden jobs affected pagination");
    check(inventory(alice, 0, job_id(more), 1) == Json{{"items", Json::array()}, {"next_cursor", nullptr}}, "job inventory end differs");
    for (std::size_t i = 1; i < scopes.size(); ++i) {
        const auto page = inventory(alice, i, 0, 1);
        check(page.at("items").size() == 1 && page.at("items")[0].at("id") == jobs[i] && page.at("next_cursor").is_null(),
              "job inventory crossed project/workspace");
    }
    const auto admin_page = inventory(admin, 0, 0, 100);
    check(admin_page.at("items").size() == 1 && admin_page.at("items")[0].at("id") == job_id(scoped),
          "scoped maintenance inventory impersonated owner or included legacy");
    for (const auto* field : {"root", "artifact_key", "artifact_namespace", "request_hash", "workspace_admission", "worker_id", "idempotency_key"})
        check(!first.at("items")[0].contains(field), "job inventory exposed private metadata");
    denied([&] { inventory(bob, 0, 0, 1); }, 404, "identity-not-found");
    denied([&] { inventory(alice, 0, 0, 0); }, 400, "invalid-limit");
    denied([&] { inventory(alice, 0, 0, 101); }, 400, "invalid-limit");
    denied([&] { inventory(alice, 0, UINT64_MAX, 1); }, 400, "invalid-cursor");

    const auto projects = reopened.identity_projects(alice, "scope-a", "", 1);
    check(projects.at("items")[0].at("project_id") == "keys-project-a" && projects.at("next_cursor") == "keys-project-a",
          "same-label project discovery failed");
    const auto workspaces = reopened.identity_workspaces(alice, "scope-a", "keys-project-a", "", 1);
    check(workspaces.at("items")[0].at("workspace_id") == "primary" && workspaces.at("next_cursor") == "primary",
          "same-label workspace discovery failed");
    check(reopened.identity_workspaces(alice, "scope-a", "keys-project-a", "primary", 1).at("items")[0].at("workspace_id") == "secondary",
          "second same-label workspace hidden");

    // Declared ledger fixtures only: no artifact is created or qualified here.
    {
        pqxx::connection db(config.database.connection_string);
        pqxx::work tx(db);
        tx.exec("SELECT set_config('shin.preparation_ops','1',true)");
        tx.exec("UPDATE shin_server.preparations SET state='prepared',worker_id='key-fixture-worker',"
            "artifact_snapshot_id='fnv1a64:0123456789abcdef',artifact_bytes=1 "
            "WHERE project_id IN ('keys-project-a','keys-project-b')");
        tx.commit();
    }
    const auto source = [&](const WorkspaceWriteScope& scope) {
        return Json{{"source_route_id", scope.route_id}, {"workspace_id", scope.workspace.workspace_id},
            {"source_host_id", config.database.source_host_id}, {"profile_id", "generic-docs"}, {"route_fingerprint", fingerprint}};
    };
    const auto submit = [&](std::size_t i) {
        const auto& scope = scopes[i];
        return store.submit_publication_proposal(alice, scope.project_id, "publication-" + scope.workspace.workspace_id,
            jobs[i], 0, "same-key", hash, source(scope), 0, scope);
    };
    for (std::size_t i = 0; i < pending.size(); ++i)
        pending[i] = std::async(std::launch::async, [&, i] { return submit(i % 2); });
    std::array<std::uint64_t, 3> proposals{};
    created = 0;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto result = pending[i].get();
        if (!result.at("replayed").get<bool>()) ++created;
        if (i < 2) proposals[i] = proposal_id(result);
        else check(proposal_id(result) == proposals[i % 2], "concurrent publication replay changed proposal");
    }
    check(created == 2 && proposals[0] != proposals[1], "same-owner workspace publication collision");
    const auto third_proposal = submit(2);
    proposals[2] = proposal_id(third_proposal);
    check(third_proposal.at("replayed") == false && proposals[2] != proposals[0] && proposals[2] != proposals[1], "same-owner project publication collision");
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        const auto& scope = scopes[i];
        const auto replay = [&](const Principal& actor, const std::string& request_hash) {
            return reopened.publication_submission_replay(actor, scope.project_id, "publication-" + scope.workspace.workspace_id,
                jobs[i], 0, "same-key", request_hash, 0, scope);
        };
        check(proposal_id(replay(alice, hash)) == proposals[i], "reopened publication replay lost");
        check(submit(i).at("replayed") == true, "publication submission replay lost");
        denied([&] { replay(alice, std::string(64, 'd')); }, 409, "idempotency-conflict");
        denied([&] { replay(bob, hash); }, 404, "identity-not-found");
    }
    const auto legacy_submit = [&] { return store.submit_publication_proposal(admin, maintenance.project_id, "legacy-publication",
        job_id(legacy), 0, "legacy-overlap", hash, source(maintenance)); };
    const auto scoped_submit = [&] { return store.submit_publication_proposal(admin, maintenance.project_id, "scoped-publication",
        job_id(scoped), 0, "legacy-overlap", hash, source(maintenance), 0, maintenance); };
    check(proposal_id(legacy_submit()) != proposal_id(scoped_submit()), "legacy publication collided with scoped request");
    check(legacy_submit().at("replayed") == true && scoped_submit().at("replayed") == true, "split publication replay lost");
    const auto before_revoke = inventory(alice, 0, 0, 1);
    const auto revision = scopes[0].admission.at("authorization").at("account_revision").get<std::uint64_t>();
    store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", revision}});
    denied([&] { inventory(alice, 0, 0, 1); }, 404, "identity-not-found");
    denied([&] { reopened.identity_projects(alice, "scope-a", "", 1); }, 404, "identity-not-found");
    denied([&] { reopened.identity_workspaces(alice, "scope-a", "keys-project-a", "", 1); }, 404, "identity-not-found");
    store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", revision + 1}});
    check(inventory(alice, 0, 0, 1) == before_revoke, "restored owner lost job history");
    return checks;
}
