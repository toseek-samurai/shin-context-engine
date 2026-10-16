#include "scoped_promotion_checks.h"
#include "shin/server/publication_service.h"
#include "shin/server/retrieval_service.h"
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
        if (error.status != status || error.code != code)
            throw std::runtime_error("unexpected promotion denial: " + error.code + " after check " + std::to_string(checks));
        ++checks;
        return;
    }
    throw std::runtime_error("promotion unexpectedly admitted");
}
}

unsigned check_scoped_promotions(const shin::server::Config& config, std::uint64_t alice_job, std::uint64_t bob_job) {
    checks = 0;
    Store store(config.database);
    RouteCatalog catalog(config);
    PreparationService preparations(config, catalog, store);
    PublicationService publications(config, catalog, store, preparations);
    RetrievalService retrieval(config, catalog);
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}};
    const WorkspaceReadRequest own{"scope-a", "primary"}, shared{"scope-b", "primary"};
    auto a = store.capture_workspace_write(alice, "visible", own);
    a.route_id = "visible-retrieval";
    auto b = store.capture_workspace_write(bob, "other", shared);
    b.route_id = "other-retrieval";
    const auto base_a = store.scoped_active_route(alice, catalog.find("visible", a.route_id).route, own);
    const auto base_b = store.scoped_active_route(bob, catalog.find("other", b.route_id).route, shared);
    const auto cleanup = [&] {
        // Fixture-only emergency cleanup uses explicit maintenance authority.
        for (const auto& [project, publication] : {std::pair{"visible", "scoped-promotion-a"},
                std::pair{"other", "scoped-promotion-b"}, std::pair{"visible", "scoped-promotion-mixed"}}) {
            const auto head = store.publication_head(admin, project, publication);
            const auto page = store.list_publication_proposals(admin, project, publication, 0, 64);
            for (const auto& item : page.at("items")) {
                if (item.at("resolution").is_null()) store.resolve_publication_proposal(admin, project, publication,
                    item.at("proposal").at("id").get<std::uint64_t>(), "reject", head.at("revision").get<std::uint64_t>(), nullptr);
            }
            if (head.at("state") == "active") store.withdraw_publication(admin, project, publication, head.at("revision").get<std::uint64_t>());
        }
        const auto active = store.active_route(admin, "visible", a.route_id);
        if (active.at("preparation_id") == alice_job)
            store.commit_activation(admin, "visible", a.route_id, active.at("revision").get<std::uint64_t>(), nullptr);
    };
    try {
        check(preparations.inspect(alice, alice_job, a).at("candidate_verified") == true, "scoped inspect failed");
        check(preparations.preflight(alice, alice_job, a).at("preflight_passed") == true, "scoped preflight failed");
        denied([&] { preparations.inspect(alice, bob_job, a); }, 404, "preparation-not-found");
        denied([&] { store.capture_workspace_write(alice, "other", shared); }, 404, "identity-not-found");
        denied([&] { store.commit_activation(alice, "visible", "foreign-route", 0, nullptr, a); }, 404, "identity-not-found");
        denied([&] { preparations.activate(alice, "visible", "missing-route", {{"preparation_id", nullptr}, {"expected_revision", 0}}, own); },
               404, "identity-not-found");
        denied([&] { preparations.activate(bob, "visible", a.route_id, {{"preparation_id", nullptr}, {"expected_revision", 0}},
            WorkspaceReadRequest{"scope-a", "secondary"}); }, 404, "identity-not-found");

        const auto submit = [&](const std::string& key, std::uint64_t revision) {
            const Json input{{"preparation_id", alice_job}, {"base_revision", revision}};
            const auto result = publications.submit(alice, "visible", "scoped-promotion-a", input, key, own);
            check(result.at("replayed") == false, "new scoped proposal replayed");
            check(publications.submit(alice, "visible", "scoped-promotion-a", input, key, own).at("replayed") == true, "proposal replay failed");
            return result.at("proposal").at("proposal").at("id").get<std::uint64_t>();
        };
        const auto decide = [&](std::uint64_t id, const std::string& decision, std::uint64_t revision) {
            const Json input{{"decision", decision}, {"expected_revision", revision}};
            const auto result = publications.resolve(alice, "visible", "scoped-promotion-a", id, input, own);
            check(result.at("replayed") == false && result.at("receipt").at("decision") == decision, "scoped decision failed");
            check(publications.resolve(alice, "visible", "scoped-promotion-a", id, input, own).at("replayed") == true, "decision replay failed");
        };
        decide(submit("scoped-reject", 0), "reject", 0);
        decide(submit("scoped-reprepare", 0), "request-reprepare", 0);
        decide(submit("scoped-accept", 0), "accept", 0);
        check(preparations.retention_plan(alice, alice_job, a).at("reason_codes") == Json::array({"candidate-active"}),
              "owner cleanup ignored active publication");
        denied([&] { preparations.apply_retention(alice, alice_job, {{"expected_plan_id", std::string(64, 'a')}}, a); },
               409, "candidate-active");
        check(store.cancel_preparation(alice, alice_job, a).at("state") == "prepared", "terminal cancellation mutated publication");
        check(retrieval.retrieve_workspace_publication(store, alice, "visible", "scoped-promotion-a", own, "brief",
            {{"query", "Routes"}, {"require_current", true}}).at("evidence").at("ok") == true, "published scoped retrieval failed");
        denied([&] { publications.withdraw(bob, "visible", "scoped-promotion-a", {{"expected_revision", 1}}, own); }, 404, "identity-not-found");
        for (const auto revision : {1U, 3U}) {
            const Json input{{"expected_revision", revision}};
            check(publications.withdraw(alice, "visible", "scoped-promotion-a", input, own).at("replayed") == false, "scoped withdrawal failed");
            check(publications.withdraw(alice, "visible", "scoped-promotion-a", input, own).at("replayed") == true, "withdrawal replay failed");
            denied([&] { store.scoped_publication_head(alice, "visible", "scoped-promotion-a", own); }, 404, "identity-not-found");
            if (revision == 1) decide(submit("scoped-republish", 2), "accept", 2);
        }

        // Declared foreign-workspace metadata, not a qualified secondary artifact.
        const auto source = store.preparation_inspection_input(alice, alice_job, a);
        const auto& route = catalog.find("visible", a.route_id);
        const Json foreign_source{{"source_route_id", a.route_id}, {"workspace_id", "secondary"},
            {"source_host_id", route.route.source_host_id}, {"profile_id", route.options.profile.id}, {"route_fingerprint", source.at("route_fingerprint")}};
        const auto mixed = store.submit_publication_proposal(admin, "visible", "scoped-promotion-mixed", alice_job, 0,
            "mixed-metadata", digest_hex(sha256("mixed-metadata")), foreign_source).at("proposal").at("proposal").at("id").get<std::uint64_t>();
        denied([&] { publications.submit(alice, "visible", "scoped-promotion-mixed", {{"preparation_id", alice_job}, {"base_revision", 0}}, "mixed-denied", own); },
               404, "identity-not-found");
        denied([&] { publications.resolve(alice, "visible", "scoped-promotion-mixed", mixed, {{"decision", "reject"}, {"expected_revision", 0}}, own); },
               404, "identity-not-found");
        denied([&] { publications.withdraw(alice, "visible", "scoped-promotion-mixed", {{"expected_revision", 0}}, own); }, 404, "identity-not-found");
        store.resolve_publication_proposal(admin, "visible", "scoped-promotion-mixed", mixed, "reject", 0, nullptr);

        const auto proposal_b = publications.submit(bob, "other", "scoped-promotion-b", {{"preparation_id", bob_job}, {"base_revision", 0}},
            "scoped-bob", shared).at("proposal").at("proposal").at("id").get<std::uint64_t>();
        check(preparations.retention_plan(bob, bob_job, b).at("reason_codes") == Json::array({"candidate-active"}),
              "owner cleanup ignored unresolved proposal");
        const auto checked = preparations.with_verified_candidate(bob, bob_job, [&](const Json& verified) {
            store.identity_account_state(admin, "bob", {{"enabled", false}, {"expected_revision", 3}});
            denied([&] { store.commit_activation(bob, "other", b.route_id, base_b.at("revision"), verified, b); }, 404, "identity-not-found");
            denied([&] { store.resolve_publication_proposal(bob, "other", "scoped-promotion-b", proposal_b, "accept", 0, verified, b); }, 404, "identity-not-found");
            store.identity_account_state(admin, "bob", {{"enabled", true}, {"expected_revision", 4}});
            denied([&] { store.commit_activation(bob, "other", b.route_id, base_b.at("revision"), verified, b); }, 409, "workspace-write-scope-changed");
            denied([&] { store.resolve_publication_proposal(bob, "other", "scoped-promotion-b", proposal_b, "accept", 0, verified, b); }, 409, "workspace-write-scope-changed");
            denied([&] { store.withdraw_publication(bob, "other", "scoped-promotion-b", 0, b); }, 409, "workspace-write-scope-changed");
            return Json{{"refused", true}};
        }, b);
        check(checked.at("refused") == true, "verified callback did not run");
        check(store.scoped_active_route(bob, catalog.find("other", b.route_id).route, shared) == base_b, "denied activation changed head");
        check(publications.resolve(bob, "other", "scoped-promotion-b", proposal_b,
            {{"decision", "reject"}, {"expected_revision", 0}}, shared).at("replayed") == false, "old proposal could not be rejected");
        denied([&] { preparations.preflight(bob, bob_job, store.capture_workspace_write(bob, "other", shared)); }, 404, "preparation-not-found");

        const Json activate{{"preparation_id", alice_job}, {"expected_revision", base_a.at("revision")}};
        const auto active = preparations.activate(alice, "visible", a.route_id, activate, own);
        check(preparations.retention_plan(alice, alice_job, a).at("reason_codes") == Json::array({"candidate-active"}),
              "owner cleanup ignored active route");
        denied([&] { preparations.apply_retention(alice, alice_job, {{"expected_plan_id", std::string(64, 'a')}}, a); },
               409, "candidate-active");
        check(active.at("replayed") == false, "scoped activation failed");
        check(preparations.activate(alice, "visible", a.route_id, activate, own).at("replayed") == true, "activation replay failed");
        denied([&] { preparations.activate(alice, "visible", a.route_id,
            {{"preparation_id", nullptr}, {"expected_revision", base_a.at("revision")}}, own); }, 409, "activation-conflict");
        const Json deactivate{{"preparation_id", nullptr}, {"expected_revision", active.at("receipt").at("revision")}};
        check(preparations.activate(alice, "visible", a.route_id, deactivate, own).at("replayed") == false, "scoped deactivation failed");
        check(preparations.activate(alice, "visible", a.route_id, deactivate, own).at("replayed") == true, "deactivation replay failed");

        const auto fresh_b = store.capture_workspace_write(bob, "other", shared);
        const auto legacy_history = store.publication_history(admin);
        const auto bob_history = store.publication_history(bob, fresh_b);
        const auto initial = store.publication_history(alice, a);
        const auto secondary_scope = store.capture_workspace_write(admin, "visible", {"scope-a", "secondary"});
        const auto secondary_history = store.publication_history(admin, secondary_scope);
        check(secondary_history.at("counts").at("retained") == 0 && secondary_history.at("history_epoch") == 0,
              "same-project secondary inherited another workspace history");
        check(initial.at("scope") == "workspace" && initial.at("history_epoch") == 0 &&
              initial.at("counts").at("retained") == 4 && initial.at("counts").at("eligible") == 0,
              "scoped history contains legacy/foreign rows or lost audit pins");
        check(bob_history.at("counts").at("retained") == 1, "Bob history contains Alice rows");
        denied([&] { store.publication_history(bob, a); }, 404, "identity-not-found");
        denied([&] { store.plan_publication_retention(alice, 0, a); }, 400, "invalid-limit");
        const auto before_pending = store.plan_publication_retention(alice, 256, a);
        const auto pending = submit("scoped-retention-pending", 4);
        denied([&] { publications.retire_history(alice, {{"plan", before_pending}, {"discard_confirmed", true}}, a); },
               409, "publication-history-stale");
        denied([&] { publications.retire_history(admin, {{"plan", store.plan_publication_retention(admin, 256, secondary_scope)},
            {"discard_confirmed", true}}, secondary_scope); }, 409, "publication-history-empty");
        // Dedicated fixture removes only its audit pins; production uses audit retention.
        {
            pqxx::connection db(config.database.connection_string);
            pqxx::work tx(db);
            tx.exec("SELECT set_config('shin.audit_ops','1',true),set_config('shin.identity_ops','1',true)");
            check(tx.exec("SELECT current_database(),current_user")[0][0].as<std::string>() == "shin_server_dev",
                  "unexpected retention fixture database");
            check(tx.exec("SELECT a.scope_workspace_id='primary' AND a.preparation_id IS NULL AND NOT EXISTS "
                "(SELECT 1 FROM shin_server.activation_receipts r WHERE r.principal_id='scoped-alice' "
                "AND r.scope_workspace_id IS DISTINCT FROM 'primary') FROM shin_server.active_routes a "
                "WHERE a.project_id='visible' AND a.route_id='visible-retrieval'")[0][0].as<bool>(),
                "deactivation lost persistent route or receipt ownership");
            tx.exec("DELETE FROM shin_server.audit_events WHERE publication_proposal_id IN "
                "(SELECT id FROM shin_server.publication_proposals WHERE (project_id='visible' AND publication_id='scoped-promotion-a') "
                "OR (project_id='other' AND publication_id='scoped-promotion-b'))");
            tx.commit();
        }
        const auto plan = store.plan_publication_retention(alice, 256, a);
        check(plan.at("delete_count") == 3 && plan.at("counts").at("unresolved") == 1 &&
              plan.at("counts").at("anchored") == 1 && plan.at("workspace_id") == "primary" &&
              plan.at("project_id") == "visible", "retention failed anchor/unresolved/domain protection");
        const Json retire{{"plan", plan}, {"discard_confirmed", true}};
        denied([&] { publications.retire_history(bob, retire, fresh_b); }, 409, "publication-history-stale");
        denied([&] { publications.retire_history(alice, {{"plan", plan}, {"discard_confirmed", false}}, a); }, 400, "invalid-request");
        // Another workspace advances while Alice's plan is outstanding.
        const auto bob_plan = store.plan_publication_retention(bob, 256, fresh_b);
        check(publications.retire_history(bob, {{"plan", bob_plan}, {"discard_confirmed", true}}, fresh_b)
              .at("receipt").at("history_epoch") == 1, "Bob epoch did not advance");
        check(store.plan_publication_retention(alice, 256, a) == plan, "foreign retirement invalidated Alice plan");
        const auto result = publications.retire_history(alice, retire, a);
        check(result.at("replayed") == false && result.at("receipt").at("deleted_proposals") == 3 &&
              result.at("receipt").at("history_epoch") == 1, "scoped retirement failed");
        const auto replay = publications.retire_history(alice, retire, a);
        check(replay.at("replayed") == true && replay.at("receipt") == result.at("receipt"), "retirement replay changed receipt");
        check(store.publication_history(admin) == legacy_history, "scoped retirement changed global history");
        check(store.list_publication_proposals(admin, "visible", "scoped-promotion-a", 0, 64).at("history_complete") == false,
              "maintenance page claimed complete history after scoped retirement");
        check(store.publication_history(admin, secondary_scope) == secondary_history,
              "retirement changed another workspace in the same project");
        const auto after_b = store.publication_history(bob, fresh_b);
        check(after_b.at("history_epoch") == 1 && after_b.at("retired_proposals") == 1, "Alice retirement changed Bob history");
        Store reopened(config.database);
        const auto after = reopened.publication_history(alice, a);
        check(after.at("history_epoch") == 1 && after.at("retired_proposals") == 3 &&
              after.at("counts").at("retained") == 2 && after.at("history_complete") == false,
              "scoped history did not survive reopen");
        check(store.scoped_publication_proposals(alice, "visible", "scoped-promotion-a", 0, 64, own).at("items").size() == 2,
              "retired withdrawal parent hid retained scoped provenance");
        denied([&] { publications.submit(alice, "visible", "scoped-promotion-a", {{"preparation_id", alice_job}, {"base_revision", 0}},
                                         "scoped-reject", own); }, 409, "publication-history-changed");
        const auto new_id = publications.submit(alice, "visible", "scoped-promotion-a",
            {{"preparation_id", alice_job}, {"base_revision", 4}, {"history_epoch", 1}}, "scoped-new-epoch", own)
            .at("proposal").at("proposal").at("id").get<std::uint64_t>();
        decide(pending, "reject", 4);
        decide(new_id, "reject", 4);
        store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", 9}});
        denied([&] { publications.retire_history(alice, retire, a); }, 404, "identity-not-found");
        store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", 10}});
        denied([&] { publications.retire_history(alice, retire, a); }, 409, "workspace-write-scope-changed");
        check(publications.retire_history(alice, retire, store.capture_workspace_write(alice, "visible", own)).at("replayed") == true,
              "fresh owner cannot replay retained receipt");
    } catch (...) { cleanup(); throw; }
    cleanup();
    return checks;
}
