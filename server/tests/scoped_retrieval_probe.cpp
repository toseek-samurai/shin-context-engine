#include "shin/server/active_snapshot.h"
#include "shin/server/retrieval_service.h"
#include "shin/server/route_files.h"
#include <iostream>
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
    catch (const ServiceError& error) { check(error.status == status && error.code == code, "unexpected denial"); return; }
    throw std::runtime_error("operation unexpectedly admitted");
}
void check_journal(Store& store, RetrievalService& retrieval, const Config& config, const Principal& admin,
                   const Principal& alice, const Principal& bob) {
    const WorkspaceReadRequest a{"scope-a", "primary"}, b{"scope-b", "primary"};
    auto forged = retrieval.journal_snapshot();
    forged["records"].back()["attribution"]["principal_id"] = bob.id;
    denied([&] { retrieval.capture_journal_batch(store, admin, forged); }, 409, "journal-attribution-invalid");
    const auto capture = retrieval.capture_journal(store, admin);
    check(capture.at("inserted_count") > 0, "attributed observations did not persist");
    check(retrieval.capture_journal(store, admin).at("replayed") == true, "attributed capture replay changed content");
    const auto page_a = store.scoped_retrieval_journal(alice, "visible", a, 0, 32);
    const auto page_b = store.scoped_retrieval_journal(bob, "other", b, 0, 32);
    check(!page_a.contains("coverage") && page_a.at("history_complete") == false &&
          page_a.at("items").size() > 2 && !page_b.at("items").empty(), "scoped journal lost records or exposed global coverage");
    bool saw_error = false;
    for (const auto& item : page_a.at("items")) {
        check(item.at("scope_principal_id") == alice.id && item.at("scope_tenant_id") == a.tenant_id &&
              item.at("scope_workspace_id") == a.workspace_id && item.at("project_id") == "visible" &&
              !item.contains("request") && !item.contains("instance_id") && !item.contains("sequence") &&
              !item.contains("producer_epoch") && !item.contains("record_hash"), "scoped journal leaked another domain or producer metadata");
        saw_error = saw_error || item.at("status") != 200;
    }
    check(saw_error, "authorized failures lost attribution");
    const auto id = page_a.at("items")[0].at("id").get<std::uint64_t>();
    const auto foreign_id = page_b.at("items")[0].at("id").get<std::uint64_t>();
    const auto detail = store.scoped_retrieval_journal_attempt(alice, "visible", a, id);
    check(detail.at("attempt").at("request").is_object() && !detail.at("attempt").contains("instance_id"),
          "own captured request missing or producer identity exposed");
    denied([&] { store.scoped_retrieval_journal_attempt(alice, "visible", a, foreign_id); }, 404, "journal-attempt-not-found");
    denied([&] { store.scoped_retrieval_journal_attempt(alice, "visible", a, INT64_MAX); }, 404, "journal-attempt-not-found");
    denied([&] { store.scoped_retrieval_journal(bob, "visible", a, 0, 32); }, 404, "identity-not-found");
    denied([&] { store.scoped_retrieval_journal(alice, "visible", a, 0, 33); }, 400, "invalid-query");
    const auto first = store.scoped_retrieval_journal(alice, "visible", a, 0, 1);
    const auto next = store.scoped_retrieval_journal(alice, "visible", a, first.at("next_after").get<std::uint64_t>(), 1);
    check(first.at("items")[0].at("id") < next.at("items")[0].at("id"), "scoped cursor repeated a record");
    Store reopened(config.database);
    check(reopened.scoped_retrieval_journal_attempt(alice, "visible", a, id) == detail, "private journal changed after reopen");
    const auto instance = capture.at("instance_id").get<std::string>();
    const auto plan = store.plan_journal_retention(admin, instance, capture.at("observed_through").get<std::uint64_t>());
    store.apply_journal_retention(admin, instance, {{"plan", plan}, {"discard_confirmed", true}});
    check(store.scoped_retrieval_journal(alice, "visible", a, 0, 32).at("items").empty(), "retired private history remained visible");
    const auto reclaim = store.plan_journal_reclamation(admin, capture.at("producer_epoch").get<std::uint64_t>());
    check(reclaim.at("producer_count") == 1, "journal fixture reclamation includes another producer");
    store.apply_journal_reclamation(admin, {{"plan", reclaim}, {"discard_future_confirmed", true}});
}

void run(const std::filesystem::path& path) {
    check(path.is_absolute(), "absolute private configuration required");
    auto config = load_config(path);
    config.observability.max_retained_requests = 256;
    config.observability.capture_query_projects = {"visible", "other"};
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "dedicated runtime stand required");
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    check(admin.admin, "maintenance fixture identity missing");
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}}, legacy{"scoped-legacy", false, {"visible"}};
    Store store(config.database);
    RouteCatalog catalog(config);
    RetrievalService retrieval(config, catalog);
    const WorkspaceReadRequest private_a{"scope-a", "primary"}, shared_b{"scope-b", "primary"};
    const Json query{{"query", "Routes"}, {"require_current", true}};
    auto read = [&](const Principal& actor, const std::string& project, const std::string& route, const WorkspaceReadRequest& scope) {
        return retrieval.retrieve_workspace(store, actor, project, route, scope, "brief", query);
    };
    // Warm the legacy domain before ownership is bound; scoped reads must not reuse its entry.
    (void)retrieval.retrieve(store, admin, "visible", "visible-retrieval", "brief", query);
    const auto legacy_loads = retrieval.cache_statistics(admin).at("loads").get<unsigned>();
    for (const auto& [id, principal] : {std::pair{"alice", alice.id}, std::pair{"bob", bob.id}})
        store.identity_register_account(admin, {{"id", id}, {"principal_id", principal}, {"display_name", id}});
    store.identity_register_tenant(admin, {{"id", "scope-a"}, {"owner_id", "alice"}, {"display_name", "A"}});
    store.identity_register_tenant(admin, {{"id", "scope-b"}, {"owner_id", "bob"}, {"display_name", "B"}});
    denied([&] { store.identity_membership(admin, "scope-a", {{"account_id", "bob"}, {"role", "maintainer"}, {"enabled", true}, {"expected_revision", 0}}); },
           403, "sharing-disabled");
    store.identity_bind_project(admin, "scope-a", {{"project_id", "visible"}});
    store.identity_bind_project(admin, "scope-b", {{"project_id", "other"}});
    store.identity_bind_workspace(admin, "scope-a", "visible", {{"workspace_id", "primary"}, {"owner_id", "alice"}});
    denied([&] { store.identity_bind_workspace(admin, "scope-a", "visible", {{"workspace_id", "secondary"}, {"owner_id", "bob"}}); },
           403, "sharing-disabled");
    store.identity_bind_workspace(admin, "scope-b", "other", {{"workspace_id", "primary"}, {"owner_id", "bob"}});
    check(store.scoped_workspace(alice, "visible", private_a).at("authorization").at("ownership").at("content_access") == "not-enabled",
          "public HTTP capability label invalidated persisted native admission receipts");
    check(store.identity_workspace(alice, "scope-a", "visible", "primary").at("content_access") == "scoped-routes-only",
          "public workspace metadata hides personal HTTP support");
    // Historical sharing metadata is preserved by migration, not an API grant.
    {
        pqxx::connection db(config.database.connection_string);
        pqxx::work tx(db);
        tx.exec("SELECT set_config('shin.identity_ops','1',true)");
        tx.exec("INSERT INTO shin_server.identity_memberships VALUES('scope-a','bob','maintainer',true,1),('scope-b','alice','reader',true,1)");
        tx.exec("INSERT INTO shin_server.identity_workspace_owners VALUES('scope-a','visible','secondary','bob','shared')");
        tx.commit();
    }
    const auto own = read(alice, "visible", "visible-retrieval", private_a);
    check(own.at("tenant_id") == "scope-a" && own.at("workspace_id") == "primary" && own.at("evidence").at("ok") == true,
          "scoped owner read failed");
    check(!own.at("evidence").contains("root") && !own.at("evidence").contains("out_root"), "host coordinates leaked");
    check(retrieval.cache_statistics(admin).at("loads") == legacy_loads + 1, "legacy cache domain reused by scoped read");
    const auto warm = retrieval.cache_statistics(admin);
    read(alice, "visible", "visible-retrieval", private_a);
    check(retrieval.cache_statistics(admin).at("loads") == warm.at("loads"), "scoped warm read reloaded payload");
    check(retrieval.retrieve_workspace(store, alice, "visible", "visible-retrieval", private_a, "search", query)
              .at("evidence").at("ok") == true, "scoped search failed");
    const Json task = Json::parse(R"({"schema_version":"shin.task-request.v1","request_id":"scoped-policy",
        "task":{"objective":"Retrieve route policy","must":[{"id":"policy","query":"project identity profile fingerprint"}]},
        "budget":{"max_evidence_bytes":8192,"max_records":6,"per_query_records":6,"per_document_records":2}})");
    check(retrieval.retrieve_workspace(store, alice, "visible", "visible-retrieval", private_a, "pack", {{"task_request", task}})
              .at("evidence").at("completion").at("complete") == true, "scoped Pack incomplete");
    denied([&] { retrieval.retrieve_workspace(store, alice, "visible", "visible-retrieval", private_a, "brief",
        {{"query", "Routes"}, {"tenant_id", "scope-b"}}); }, 400, "invalid-request");
    for (int i = 0; i < 8; ++i) {
        denied([&] { read(bob, "visible", "visible-retrieval", private_a); }, 404, "identity-not-found");
        denied([&] { read(alice, "other", "other-retrieval", shared_b); }, 404, "identity-not-found");
        denied([&] { read(alice, "visible", "visible-retrieval", {"scope-a", "missing"}); }, 404, "identity-not-found");
        denied([&] { read(alice, "visible", "visible-retrieval", {"scope-b", "primary"}); }, 404, "identity-not-found");
        check(read(alice, "visible", "visible-retrieval", private_a).at("tenant_id") == "scope-a", "pooled scope leaked");
    }
    denied([&] { read(legacy, "visible", "visible-retrieval", private_a); }, 404, "identity-not-found");
    denied([&] { read(bob, "visible", "visible-retrieval", {"scope-a", "secondary"}); }, 404, "identity-not-found");
    denied([&] { read(bob, "visible", "missing-route", {"scope-a", "secondary"}); }, 404, "identity-not-found");
    denied([&] { store.scoped_active_route(bob, catalog.find("visible", "visible-retrieval").route, {"scope-a", "secondary"}); },
           404, "identity-not-found");
    const auto shared = read(bob, "other", "other-retrieval", shared_b);
    check(shared.at("tenant_id") == "scope-b" && shared.at("evidence") != own.at("evidence"), "cross-tenant payload reused");
    auto before_shared = retrieval.cache_statistics(admin);
    denied([&] { read(alice, "other", "other-retrieval", shared_b); }, 404, "identity-not-found");
    check(retrieval.cache_statistics(admin) == before_shared, "historical membership touched foreign cache");
    // Hold a real native read lifetime and change its authorization before finalization.
    auto selection = [&] {
        auto selected = store.scoped_active_route(alice, catalog.find("visible", "visible-retrieval").route, private_a);
        selected["source_route_id"] = "visible-retrieval";
        return selected;
    };
    SnapshotCache cache(config.retrieval);
    const ActiveSnapshot held(config, catalog, cache, store, alice, "visible", "visible-retrieval", false,
                              selection(), route_directory_identity(config.preparations.state_dir), selection, private_a);
    store.identity_account_state(admin, "alice", {{"enabled", false}, {"expected_revision", 1}});
    check(retrieval.capture_journal(store, admin).at("inserted_count") > 0, "delayed journal capture was lost after revocation");
    denied([&] { store.scoped_retrieval_journal(alice, "visible", private_a, 0, 32); }, 404, "identity-not-found");
    denied([&] { held.verify(); }, 404, "identity-not-found");
    denied([&] { read(alice, "visible", "visible-retrieval", private_a); }, 404, "identity-not-found");
    store.identity_account_state(admin, "alice", {{"enabled", true}, {"expected_revision", 2}});
    denied([&] { held.verify(); }, 409, "workspace-read-scope-changed");
    check(read(alice, "visible", "visible-retrieval", private_a).at("evidence") == own.at("evidence"), "account restore failed");
    denied([&] { store.identity_request_admission(alice, false); }, 403, "identity-data-plane-not-enabled");
    denied([&] { store.identity_request_admission(legacy, false); }, 403, "identity-data-plane-not-enabled");
    auto publication_read = [&](const Principal& actor, const std::string& project, const WorkspaceReadRequest& scope) {
        return retrieval.retrieve_workspace_publication(store, actor, project, "scoped-publication", scope, "brief", query);
    };
    (void)retrieval.retrieve_publication(store, admin, "visible", "scoped-publication", "brief", query);
    const auto before_publication = retrieval.cache_statistics(admin);
    const auto private_publication = publication_read(alice, "visible", private_a);
    check(private_publication.at("publication_id") == "scoped-publication" && private_publication.at("tenant_id") == "scope-a" &&
          private_publication.at("evidence") == own.at("evidence"), "scoped publication evidence differs");
    check(retrieval.cache_statistics(admin).at("loads").get<unsigned>() == before_publication.at("loads").get<unsigned>() + 1,
          "scoped publication reused legacy cache domain");
    const auto before_private_hit = retrieval.cache_statistics(admin);
    publication_read(alice, "visible", private_a);
    check(retrieval.cache_statistics(admin).at("loads") == before_private_hit.at("loads"), "publication warm read reloaded snapshot");
    const auto before_publication_denials = retrieval.cache_statistics(admin);
    denied([&] { publication_read(bob, "visible", private_a); }, 404, "identity-not-found");
    denied([&] { publication_read(bob, "visible", {"scope-a", "secondary"}); }, 404, "identity-not-found");
    denied([&] { publication_read(alice, "visible", {"scope-b", "primary"}); }, 404, "identity-not-found");
    denied([&] { publication_read(legacy, "visible", private_a); }, 404, "identity-not-found");
    denied([&] { retrieval.retrieve_workspace_publication(store, alice, "visible", "missing", private_a, "brief", query); },
           404, "identity-not-found");
    check(retrieval.cache_statistics(admin) == before_publication_denials, "unauthorized publication touched cache");
    check(retrieval.retrieve_workspace_publication(store, alice, "visible", "scoped-publication", private_a, "search", query)
              .at("evidence").at("ok") == true, "scoped publication search failed");
    check(retrieval.retrieve_workspace_publication(store, alice, "visible", "scoped-publication", private_a, "pack", {{"task_request", task}})
              .at("evidence").at("completion").at("complete") == true, "scoped publication Pack failed");
    const auto shared_publication = publication_read(bob, "other", shared_b);
    const auto before_shared_publication = retrieval.cache_statistics(admin);
    denied([&] { publication_read(alice, "other", shared_b); }, 404, "identity-not-found");
    check(retrieval.cache_statistics(admin) == before_shared_publication, "private publication touched foreign cache");
    const auto select_publication = [&] { return store.scoped_publication_head(bob, "other", "scoped-publication", shared_b); };
    const ActiveSnapshot held_publication(config, catalog, cache, store, bob, "other", "scoped-publication", true,
        select_publication(), route_directory_identity(config.preparations.state_dir), select_publication, shared_b);
    store.identity_account_state(admin, "bob", {{"enabled", false}, {"expected_revision", 1}});
    denied([&] { held_publication.verify(); }, 404, "identity-not-found");
    denied([&] { publication_read(bob, "other", shared_b); }, 404, "identity-not-found");
    store.identity_account_state(admin, "bob", {{"enabled", true}, {"expected_revision", 2}});
    denied([&] { held_publication.verify(); }, 409, "workspace-read-scope-changed");
    const ActiveSnapshot before_withdrawal(config, catalog, cache, store, bob, "other", "scoped-publication", true,
        select_publication(), route_directory_identity(config.preparations.state_dir), select_publication, shared_b);
    store.withdraw_publication(admin, "other", "scoped-publication", shared_publication.at("revision").get<std::uint64_t>());
    denied([&] { before_withdrawal.verify(); }, 404, "identity-not-found");
    const auto after_withdrawal = retrieval.cache_statistics(admin);
    denied([&] { publication_read(bob, "other", shared_b); }, 404, "identity-not-found");
    denied([&] { publication_read(alice, "other", shared_b); }, 404, "identity-not-found");
    check(retrieval.cache_statistics(admin) == after_withdrawal, "withdrawn publication accessed warm cache");
    check(publication_read(alice, "visible", private_a).at("evidence") == private_publication.at("evidence"), "other tenant withdrawal leaked");
    check_journal(store, retrieval, config, admin, alice, bob);
    denied([&] { store.identity_request_admission(alice, false); }, 403, "identity-data-plane-not-enabled");
    std::cout << Json{{"ok", true}, {"checks", checks}, {"scoped_publications", true},
        {"transport", "native-internal-http-still-fenced"}}.dump() << '\n';
}

void run_history(const std::filesystem::path& path, const Json& fixture) {
    const auto config = load_config(path);
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "dedicated runtime stand required");
    Principal admin;
    for (const auto& credential : config.credentials) if (credential.principal.admin) admin = credential.principal;
    check(admin.admin, "maintenance fixture identity missing");
    const Principal alice{"scoped-alice", false, {}}, bob{"scoped-bob", false, {}}, legacy{"scoped-legacy", false, {"visible"}};
    Store store(config.database);
    const WorkspaceReadRequest own{"scope-a", "primary"}, secondary{"scope-a", "secondary"}, shared{"scope-b", "primary"};
    const auto detail = [&](const Principal& actor, const std::string& project, const WorkspaceReadRequest& scope, const char* id) {
        return store.scoped_publication_proposal(actor, project, "scoped-publication", fixture.at(id).get<std::uint64_t>(), scope);
    };
    const auto page = [&](const Principal& actor, const std::string& project, const WorkspaceReadRequest& scope,
                          std::uint64_t after, std::size_t limit) {
        return store.scoped_publication_proposals(actor, project, "scoped-publication", after, limit, scope);
    };
    const auto first = page(alice, "visible", own, 0, 1);
    check(first.at("items").size() == 1 && first.at("items")[0].at("proposal").at("id") == fixture.at("private_seed"), "private first page differs");
    check(first.at("next_after") == fixture.at("private_seed"), "private cursor differs");
    check(!first.contains("history_epoch") && !first.contains("history_complete"), "global history metadata leaked");
    const auto second = page(alice, "visible", own, first.at("next_after").get<std::uint64_t>(), 1);
    check(second.at("items").size() == 1 && second.at("items")[0].at("proposal").at("id") == fixture.at("visible_one"),
          "hidden rows displaced visible page");
    check(second.at("next_after") == fixture.at("visible_one"), "visible continuation differs");
    const auto last = page(alice, "visible", own, second.at("next_after").get<std::uint64_t>(), 1);
    check(last.at("items").size() == 1 && last.at("items")[0].at("proposal").at("id") == fixture.at("visible_two") && last.at("next_after").is_null(),
          "last visible page differs");
    for (const auto* key : {"private_seed", "visible_one", "visible_two"})
        check(detail(alice, "visible", own, key).at("candidate").at("workspace_id") == "primary", "authorized history refused");
    for (const auto* key : {"hidden_base", "unknown_base", "foreign_candidate"})
        denied([&] { detail(alice, "visible", own, key); }, 404, "identity-not-found");
    check(detail(admin, "visible", own, "hidden_base").at("base").at("workspace_id") == "secondary", "maintenance cannot inspect mixed history");
    denied([&] { detail(bob, "visible", secondary, "foreign_candidate"); }, 404, "identity-not-found");
    denied([&] { page(bob, "visible", secondary, 0, 64); }, 404, "identity-not-found");
    check(detail(admin, "visible", secondary, "foreign_candidate").at("candidate").at("workspace_id") == "secondary", "candidate fixture unavailable");
    denied([&] { page(legacy, "visible", own, 0, 1); }, 404, "identity-not-found");
    denied([&] { page(bob, "visible", own, 0, 1); }, 404, "identity-not-found");
    denied([&] { page(alice, "visible", {"scope-b", "primary"}, 0, 1); }, 404, "identity-not-found");
    denied([&] { store.scoped_publication_proposal(alice, "visible", "scoped-publication", 9223372036854775806ULL, own); }, 404, "identity-not-found");
    check(store.scoped_publication_proposals(alice, "visible", "missing", 0, 1, own).at("items").empty(), "missing publication enumerated");
    const auto shared_page = page(bob, "other", shared, 0, 64);
    check(shared_page.at("items").size() == 2, "withdrawn history lost authorized records");
    check(detail(bob, "other", shared, "withdrawn_base").at("base").at("state") == "withdrawn", "withdrawn base proof missing");
    denied([&] { page(alice, "other", shared, 0, 64); }, 404, "identity-not-found");
    denied([&] { detail(alice, "other", shared, "withdrawn_base"); }, 404, "identity-not-found");
    check(page(bob, "other", shared, 0, 64) == shared_page, "foreign read changed owner history");
    for (int i = 0; i < 4; ++i) {
        denied([&] { detail(bob, "visible", secondary, "foreign_candidate"); }, 404, "identity-not-found");
        check(page(alice, "visible", own, 0, 64).at("items").size() == 3, "pooled history scope leaked");
    }
    for (const auto limit : {0U, 65U}) denied([&] { page(alice, "visible", own, 0, limit); }, 400, "invalid-limit");
    denied([&] { page(alice, "visible", own, UINT64_MAX, 1); }, 400, "invalid-cursor");
    denied([&] { store.identity_request_admission(alice, false); }, 403, "identity-data-plane-not-enabled");
    std::cout << Json{{"ok", true}, {"checks", checks}, {"scoped_history", true}}.dump() << '\n';
}
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    try {
        if (argc == 3) run_history(argv[1], shin::server::Json::parse(argv[2]));
        else run(argv[1]);
    }
    catch (const shin::server::ServiceError& error) { std::cerr << error.code << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
