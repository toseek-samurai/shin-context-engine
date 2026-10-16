#include "preparation_access.h"
#include "legacy_project_access.h"
#include "route_access.h"

namespace shin::server {
namespace {
std::string begin_ops(pqxx::transaction_base& tx) {
    const auto previous = tx.exec("SELECT current_setting('shin.preparation_ops',true)")[0][0].as<std::string>("");
    preparation_worker_scope(tx);
    return previous;
}
void end_ops(pqxx::transaction_base& tx, const std::string& previous) {
    tx.exec("SELECT set_config('shin.preparation_ops',$1,true)", pqxx::params{tx, previous});
}
}

void legacy_preparation_scope(pqxx::transaction_base& tx, const Principal& actor) {
    legacy_access::validate_principal(actor);
    std::string projects;
    for (const auto& project : actor.project_ids) {
        if (!projects.empty()) projects += ',';
        projects += project;
    }
    tx.exec("SELECT set_config('shin.identity_principal',$1,true), set_config('shin.identity_ops',$2,true), "
            "set_config('shin.identity_legacy_projects',$3,true), set_config('shin.identity_account','',true), "
            "set_config('shin.identity_tenant','',true), set_config('shin.identity_manage','',true), "
            "set_config('shin.content_project','',true), set_config('shin.content_workspace','',true), "
            "set_config('shin.content_route','',true), "
            "set_config('shin.preparation_sources','',true), set_config('shin.preparation_ops','0',true), "
            "set_config('shin.preparation_legacy','1',true)", pqxx::params{tx, actor.id, actor.admin ? "1" : "0", projects});
}

void preparation_worker_scope(pqxx::transaction_base& tx) {
    // Internal workers must retire revoked/expired jobs; this does not grant directory or checkpoint access.
    tx.exec("SELECT set_config('shin.preparation_ops','1',true)");
}

pqxx::result preparation_capacity(pqxx::transaction_base& tx) {
    // Caller holds the global admission lock. Failure aborts the transaction, including this local authority.
    const auto previous = begin_ops(tx);
    const auto result = tx.exec(
        "SELECT (SELECT count(*)::bigint FROM shin_server.preparations) AS history, "
        "count(*)::bigint AS retained, COALESCE(sum(p.reserved_artifact_bytes),0)::bigint AS reserved "
        "FROM shin_server.preparations p WHERE NOT EXISTS (SELECT 1 FROM shin_server.preparation_retention r "
        "WHERE r.preparation_id=p.id AND r.state='released')");
    end_ops(tx, previous);
    return result;
}

void preparation_route_source(pqxx::transaction_base& tx, const std::string& project, const std::string& route) {
    select_content_route(tx, route);
    tx.exec("SELECT set_config('shin.preparation_sources',COALESCE(current_setting('shin.preparation_sources',true),'')||','||COALESCE((SELECT preparation_id::text "
            "FROM shin_server.active_routes WHERE project_id=$1 AND route_id=$2),''),true)", pqxx::params{tx, project, route});
}

void preparation_publication_sources(pqxx::transaction_base& tx, const std::string& project, const std::string& publication) {
    // Retained history may refer to separately owned workspaces: admit each explicit source through live directory RLS.
    tx.exec("SELECT set_config('shin.preparation_sources',COALESCE(current_setting('shin.preparation_sources',true),'')||','||"
            "COALESCE(string_agg(DISTINCT source.id::text,','),''),true) "
            "FROM shin_server.publication_proposals p CROSS JOIN LATERAL "
            "(VALUES (p.preparation_id,p.workspace_id),(p.base_preparation_id,p.base_workspace_id)) source(id,workspace) "
            "JOIN shin_server.identity_workspace_owners w ON w.project_id=p.project_id AND w.workspace_id=source.workspace "
            "AND w.tenant_id=current_setting('shin.identity_tenant',true) "
            "WHERE p.project_id=$1 AND p.publication_id=$2", pqxx::params{tx, project, publication});
}

void lock_preparation_inputs(pqxx::transaction_base& tx, const std::string& project,
                             std::uint64_t candidate, std::optional<std::int64_t> base) {
    const pqxx::result::size_type expected = base && *base != static_cast<std::int64_t>(candidate) ? 2 : 1;
    const auto sql = "SELECT id FROM shin_server.preparations WHERE project_id=$1 AND (id=$2 OR id=$3) ORDER BY id";
    const auto params = [&] { return pqxx::params{tx, project, candidate, base}; };
    if (tx.exec(sql, params()).size() != expected)
        throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
    // Source-only SELECT grants cannot mutate rows. Acquire only these already visible identities, without returning payload.
    const auto previous = begin_ops(tx);
    const auto locked = tx.exec(std::string(sql) + " FOR UPDATE", params());
    end_ops(tx, previous);
    if (locked.size() != expected)
        throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
}
}
