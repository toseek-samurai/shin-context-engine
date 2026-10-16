#include "personal_route_checks.h"
#include "shin/server/preparation_service.h"
#include "shin/server/retrieval_service.h"
#include <array>
#include <chrono>
#include <thread>
#include <pqxx/pqxx>

namespace {
using namespace shin::server;
struct Source {
    Principal actor;
    std::string project;
    std::string route;
    WorkspaceReadRequest scope;
    std::uint64_t job = 0;
    Json base;
};
Json publication_fingerprint(const Config& config) {
    pqxx::connection db(config.database.connection_string);
    pqxx::read_transaction tx(db);
    Json result = Json::array();
    for (const auto* table : {"publication_proposals", "publication_resolutions", "publication_heads",
                              "publication_withdrawals", "workspace_publication_history"}) {
        const auto rows = tx.exec("SELECT md5(COALESCE(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text)::text,'')) "
                                 "FROM shin_server." + std::string(table) + " t");
        result.push_back(rows[0][0].as<std::string>());
    }
    return result;
}
}

unsigned check_personal_routes(const shin::server::Config& config) {
    unsigned checks = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
        ++checks;
    };
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "personal route fixture requires the dedicated stand");
    std::array<Source, 2> sources{{
        {{"scoped-alice", false, {}}, "visible", "visible-retrieval", {"scope-a", "primary"}, 0, Json()},
        {{"scoped-bob", false, {}}, "other", "other-retrieval", {"scope-b", "primary"}, 0, Json()}}};
    Store store(config.database);
    RouteCatalog catalog(config);
    PreparationService operations(config, catalog, store);
    const auto before = publication_fingerprint(config);
    const auto cleanup = [&] {
        for (const auto& source : sources) {
            if (!source.job) continue;
            const auto active = store.scoped_active_route(source.actor, catalog.find(source.project, source.route).route, source.scope);
            if (active.at("preparation_id") == source.job)
                operations.activate(source.actor, source.project, source.route,
                    {{"preparation_id", nullptr}, {"expected_revision", active.at("revision")}}, source.scope);
            const auto scope = store.capture_workspace_write(source.actor, source.project, source.scope);
            store.cancel_preparation(source.actor, source.job, scope);
            const auto plan = operations.retention_plan(source.actor, source.job, scope);
            if (plan.at("state") != "released") {
                check(plan.at("can_apply") == true, "personal candidate required publication cleanup");
                check(operations.apply_retention(source.actor, source.job, {{"expected_plan_id", plan.at("plan_id")}}, scope)
                    .at("state") == "released", "personal candidate cleanup failed");
            }
        }
    };
    try {
        for (auto& source : sources) {
            source.base = store.scoped_active_route(source.actor, catalog.find(source.project, source.route).route, source.scope);
            const auto queued = operations.enqueue(source.actor, source.project, source.route,
                {{"expected_snapshot_id", source.base.at("snapshot_id")}}, "personal-no-publication", source.scope);
            source.job = queued.at("job").at("id").get<std::uint64_t>();
            check(!queued.at("replayed").get<bool>(), "personal fixture unexpectedly replayed");
        }
        {
            PreparationService worker(config, catalog, store);
            worker.start();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            for (;;) {
                bool prepared = true;
                for (const auto& source : sources) {
                    const auto job = store.scoped_preparation(source.actor, source.project, source.job, source.scope);
                    if (job.at("state") == "failed") throw std::runtime_error("personal worker failed");
                    prepared = prepared && job.at("state") == "prepared";
                }
                if (prepared) break;
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("personal worker deadline exceeded");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        for (const auto& source : sources) {
            const Json request{{"preparation_id", source.job}, {"expected_revision", source.base.at("revision")}};
            check(operations.activate(source.actor, source.project, source.route, request, source.scope).at("replayed") == false,
                  "personal route activation failed");
            check(operations.activate(source.actor, source.project, source.route, request, source.scope).at("replayed") == true,
                  "personal route activation replay failed");
        }
        // Reopen the pool/reader: activation is durable without a named head.
        Store reopened(config.database);
        RetrievalService retrieval(config, catalog);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i];
            for (int pass = 0; pass < 2; ++pass) {
                const auto result = retrieval.retrieve_workspace(reopened, source.actor, source.project, source.route,
                    source.scope, "brief", {{"query", "Routes"}, {"require_current", true}});
                check(result.at("preparation_id") == source.job && result.at("workspace_id") == source.scope.workspace_id &&
                      result.at("tenant_id") == source.scope.tenant_id && result.at("evidence").at("ok") == true &&
                      !result.contains("publication_id"), "personal read selected another domain or named publication");
            }
            try {
                retrieval.retrieve_workspace(reopened, sources[1 - i].actor, source.project, source.route,
                    source.scope, "brief", {{"query", "Routes"}, {"require_current", true}});
                throw std::runtime_error("foreign personal route admitted");
            } catch (const ServiceError& error) {
                check(error.status == 404 && error.code == "identity-not-found", "foreign route disclosed authority");
            }
        }
        check(publication_fingerprint(config) == before, "personal work mutated named publication state");
    } catch (...) { cleanup(); throw; }
    cleanup();
    check(publication_fingerprint(config) == before, "personal cleanup changed retained publication history");
    return checks;
}
